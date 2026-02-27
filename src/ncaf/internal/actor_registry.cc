#include "ncaf/internal/actor_registry.h"

#include <exception>

#include "ncaf/internal/remote_handler_registry.h"

namespace ncaf::internal {

ActorRegistryRequestProcessor::ActorRegistryRequestProcessor(std::unique_ptr<TypeErasedActorScheduler> scheduler,
                                                             uint32_t this_node_id,
                                                             const std::vector<NodeInfo>& cluster_node_info,
                                                             network::MessageBroker* message_broker)
    : is_distributed_mode_(!cluster_node_info.empty()),
      scheduler_(std::move(scheduler)),
      this_node_id_(this_node_id),
      message_broker_(message_broker) {
  InitRandomNumGenerator();
  ValidateNodeInfo(cluster_node_info);
}

exec::task<void> ActorRegistryRequestProcessor::AsyncDestroyAllActors() {
  exec::async_scope async_scope;
  logging::Info("Sending destroy messages to actors");
  for (auto& [_, actor] : actor_id_to_actor_) {
    async_scope.spawn(actor->AsyncDestroy());
  }
  logging::Info("Waiting for actors to be destroyed");
  co_await async_scope.on_empty();
  logging::Info("All actors destroyed");
}

// 分发并处理网络请求
exec::task<void> ActorRegistryRequestProcessor::HandleNetworkRequest(uint64_t received_request_id,
                                                                     network::ByteBufferType request_buffer) {
  serde::BufferReader<network::ByteBufferType> reader(std::move(request_buffer));
  auto message_type = reader.NextPrimitive<serde::NetworkRequestType>();

  // 根据消息类型调用不同的处理函数
  if (message_type == serde::NetworkRequestType::kActorCreationRequest) {
    HandleActorCreationRequest(received_request_id, std::move(reader));
    co_return;
  }

  if (message_type == serde::NetworkRequestType::kActorMethodCallRequest) {
    co_await HandleActorMethodCallRequest(received_request_id, std::move(reader));
    co_return;
  }

  // 如果是查找请求
  if (message_type == serde::NetworkRequestType::kActorLookUpRequest) {
    // 反序列出请求的actor_name
    auto actor_name =
        serde::Deserialize<serde::ActorLookUpRequest>(reader.Current(), reader.RemainingSize()).actor_name;

    // 如果在本地找到了这个actor_name，回复actor_id,否则回复错误
    if (actor_name_to_id_.contains(actor_name)) {
      serde::BufferWriter<network::ByteBufferType> writer(
          network::ByteBufferType(sizeof(serde::NetworkRequestType) + sizeof(uint64_t)));
      auto actor_id = actor_name_to_id_.at(actor_name);
      writer.WritePrimitive(serde::NetworkReplyType::kActorLookUpReturn);
      writer.WritePrimitive(actor_id);
      message_broker_->ReplyRequest(received_request_id, std::move(writer).MoveBufferOut());
    } else {
      serde::BufferWriter writer(network::ByteBufferType(sizeof(serde::NetworkRequestType)));
      writer.WritePrimitive(serde::NetworkReplyType::kActorLookUpError);
      message_broker_->ReplyRequest(received_request_id, std::move(writer).MoveBufferOut());
    }
    co_return;
  }
  NCAF_THROW << "Invalid message type: " << static_cast<int>(message_type);
}

void ActorRegistryRequestProcessor::InitRandomNumGenerator() {
  std::random_device rd;
  random_num_generator_ = std::mt19937(rd());
}

uint64_t ActorRegistryRequestProcessor::GenerateRandomActorId() {
  while (true) {
    auto id = random_num_generator_();
    if (!actor_id_to_actor_.contains(id)) {
      return id;
    }
  }
}

void ActorRegistryRequestProcessor::ValidateNodeInfo(const std::vector<NodeInfo>& cluster_node_info) {
  for (const auto& node : cluster_node_info) {
    NCAF_THROW_CHECK(!node_id_to_address_.contains(node.node_id)) << "Duplicate node id: " << node.node_id;
    node_id_to_address_[node.node_id] = node.address;
  }
}

serde::NetworkRequestType ActorRegistryRequestProcessor::ParseMessageType(const network::ByteBufferType& buffer) {
  NCAF_THROW_CHECK_LE(buffer.size(), 1) << "Invalid buffer size, " << buffer.size();
  return static_cast<serde::NetworkRequestType>(*static_cast<const uint8_t*>(buffer.data()));
}

void ActorRegistryRequestProcessor::ReplyError(uint64_t received_request_id, serde::NetworkReplyType reply_type,
                                               std::string error_msg) {
  std::vector<char> serialized = serde::Serialize(serde::ActorMethodReturnError {std::move(error_msg)});
  serde::BufferWriter writer(network::ByteBufferType(sizeof(serde::NetworkRequestType) + serialized.size()));
  writer.WritePrimitive(reply_type);
  writer.CopyFrom(serialized.data(), serialized.size());
  message_broker_->ReplyRequest(received_request_id, std::move(writer).MoveBufferOut());
}

// 处理actor创建请求
void ActorRegistryRequestProcessor::HandleActorCreationRequest(uint64_t received_request_id,
                                                               serde::BufferReader<network::ByteBufferType> reader) {
  // 读取长度
  auto handler_key_len = reader.NextPrimitive<uint64_t>();
  // 读取调用的方法名
  auto handler_key = reader.PullString(handler_key_len);
  // 根据方法名从远程actor处理器注册表里获取对应的handler
  try {
    // 获取到创建远程actor的handler
    auto handler = RemoteActorRequestHandlerRegistry::GetInstance().GetRemoteActorCreationHandler(handler_key);
    ActorRefDeserializationInfo info {.this_node_id = this_node_id_,
                                      .actor_look_up_fn = [&](uint64_t actor_id) -> TypeErasedActor* {
                                        if (actor_id_to_actor_.contains(actor_id)) {
                                          return actor_id_to_actor_.at(actor_id).get();
                                        }
                                        return nullptr;
                                      },
                                      .message_broker = message_broker_};
    // 调用handler,handle内部就会创建actor
    auto result = handler(RemoteActorRequestHandlerRegistry::RemoteActorCreationHandlerContext {
        .request_buffer = std::move(reader), .scheduler = scheduler_->Clone(), .info = info});
    // 随机生成一个actor_id
    uint64_t actor_id = GenerateRandomActorId();
    // 如果创建的actor有名字，注册名字和actor_id的映射关系
    if (result.actor_name.has_value()) {
      NCAF_THROW_CHECK(!actor_name_to_id_.contains(result.actor_name.value()))
          << "An actor with the same name already exists, name=" << result.actor_name.value();
      actor_name_to_id_[result.actor_name.value()] = actor_id;
    }
    // 注册actor_id和actor实例的映射关系
    actor_id_to_actor_[actor_id] = std::move(result.actor);
    // 回复创建结果，包含actor_id
    serde::BufferWriter<network::ByteBufferType> writer(
        network::ByteBufferType(sizeof(serde::NetworkReplyType) + sizeof(actor_id)));
    writer.WritePrimitive(serde::NetworkReplyType::kActorCreationReturn);
    writer.WritePrimitive(actor_id);
    message_broker_->ReplyRequest(received_request_id, std::move(writer).MoveBufferOut());
  } catch (std::exception& error) {
    auto error_msg = fmt_lib::format("Exception type: {}, what(): {}", typeid(error).name(), error.what());
    ReplyError(received_request_id, serde::NetworkReplyType::kActorCreationError, std::move(error_msg));
  }
}

// 处理actor方法调用请求
exec::task<void> ActorRegistryRequestProcessor::HandleActorMethodCallRequest(
    uint64_t received_request_id, serde::BufferReader<network::ByteBufferType> reader) {
  auto handler_key_len = reader.NextPrimitive<uint64_t>();
  auto handler_key = reader.PullString(handler_key_len);
  auto actor_id = reader.NextPrimitive<uint64_t>();
  if (!actor_id_to_actor_.contains(actor_id)) {
    ReplyError(
        received_request_id, serde::NetworkReplyType::kActorMethodCallError,
        fmt_lib::format("Can't find actor at remote node, actor_id={}, node_id={}, maybe it's already destroyed.",
                        actor_id, this_node_id_));
    co_return;
  }

  RemoteActorRequestHandlerRegistry::RemoteActorMethodCallHandler handler = nullptr;
  try {
    handler = RemoteActorRequestHandlerRegistry::GetInstance().GetRemoteActorMethodCallHandler(handler_key);
  } catch (std::exception& error) {
    auto error_msg = fmt_lib::format("Exception type: {}, what(): {}", typeid(error).name(), error.what());
    ReplyError(received_request_id, serde::NetworkReplyType::kActorMethodCallError, std::move(error_msg));
    co_return;
  }

  NCAF_THROW_CHECK(handler != nullptr);
  ActorRefDeserializationInfo info {.this_node_id = this_node_id_,
                                    .actor_look_up_fn = [&](uint64_t actor_id) -> TypeErasedActor* {
                                      if (actor_id_to_actor_.contains(actor_id)) {
                                        return actor_id_to_actor_.at(actor_id).get();
                                      }
                                      return nullptr;
                                    },
                                    .message_broker = message_broker_};
  try {
    auto task = handler(RemoteActorRequestHandlerRegistry::RemoteActorMethodCallHandlerContext {
        .actor = actor_id_to_actor_.at(actor_id).get(), .request_buffer = std::move(reader), .info = info});
    auto buffer = co_await std::move(task);
    message_broker_->ReplyRequest(received_request_id, std::move(buffer));
  } catch (std::exception& error) {
    auto error_msg = fmt_lib::format("Exception type: {}, what(): {}", typeid(error).name(), error.what());
    ReplyError(received_request_id, serde::NetworkReplyType::kActorMethodCallError, std::move(error_msg));
  }
}

// ----------------------ActorRegistry--------------------------
ActorRegistry::~ActorRegistry() {
  logging::Info("Start to shutdown actor registry");
  if (is_distributed_mode_) {
    message_broker_->ClusterAlignedStop();
  }
  ex::sync_wait(processor_actor_.CallActorMethod<&ActorRegistryRequestProcessor::AsyncDestroyAllActors>());
  ex::sync_wait(processor_actor_.AsyncDestroy());
  ex::sync_wait(async_scope_.on_empty());
  logging::Info("Actor registry shutdown completed");
}

ActorRegistry::ActorRegistry(uint32_t thread_pool_size, std::unique_ptr<TypeErasedActorScheduler> scheduler,
                             uint32_t this_node_id, const std::vector<NodeInfo>& cluster_node_info,
                             network::HeartbeatConfig heartbeat_config)
    : is_distributed_mode_(!cluster_node_info.empty()),
      this_node_id_(this_node_id),
      default_work_sharing_thread_pool_(thread_pool_size),
      scheduler_(scheduler != nullptr ? std::move(scheduler)
                                      : std::make_unique<AnyStdExecScheduler<WorkSharingThreadPool::Scheduler>>(
                                            default_work_sharing_thread_pool_.GetScheduler())),
      message_broker_([&cluster_node_info, &heartbeat_config, this]() -> std::unique_ptr<network::MessageBroker> {
        if (cluster_node_info.empty()) {
          return nullptr;
        }
        return std::make_unique<network::MessageBroker>(
            cluster_node_info, this_node_id_,
            /*request_handler=*/
            [this](uint64_t received_request_id, network::ByteBufferType data) {
              auto task = processor_actor_.CallActorMethod<&ActorRegistryRequestProcessor::HandleNetworkRequest>(
                  received_request_id, std::move(data));
              async_scope_.spawn(std::move(task));
            },
            heartbeat_config);
      }()),
      processor_actor_(scheduler_->Clone(), ActorConfig {.node_id = this_node_id_}, scheduler_->Clone(), this_node_id,
                       cluster_node_info, message_broker_.get()),
      processor_actor_ref_(this_node_id_, this_node_id_, /*actor_id=*/UINT64_MAX, &processor_actor_,
                           message_broker_.get()) {}
}  // namespace ncaf::internal

// ----------------------Global Default Registry--------------------------

namespace {
std::vector<std::shared_ptr<void>> resource_holder;            // 用于持有资源，确保在全局默认注册表销毁前资源不会被销毁
std::unique_ptr<ncaf::ActorRegistry> global_default_registry;  // 全局默认注册表的指针
}  // namespace

namespace ncaf::internal {
ncaf::ActorRegistry& GetGlobalDefaultRegistry() {
  NCAF_THROW_CHECK(IsGlobalDefaultRegistryInitialized()) << "Global default registry is not initialized.";
  return *global_default_registry;
}

void AssignGlobalDefaultRegistry(std::unique_ptr<ncaf::ActorRegistry> registry) {
  global_default_registry = std::move(registry);
}

bool IsGlobalDefaultRegistryInitialized() { return global_default_registry != nullptr; }

// 清理函数，在main函数退出时如果全局默认注册表还没有被销毁，说明用户没有正确调用Shutdown函数，强制退出程序并打印错误日志
static void RegisterAtExitCleanup() {
  static bool at_exit_cleanup_registered = false;

  if (at_exit_cleanup_registered) {
    return;
  }
  at_exit_cleanup_registered = true;

  std::atexit([] {
    if (!IsGlobalDefaultRegistryInitialized()) {
      return;
    }
    logging::Error(
        "ncaf is not shutdown when exiting main(), calling std::quick_exit(1) to force exit, resources may not be "
        "cleaned properly. To fix this error, call ncaf::Shutdown() before main() exits.");
    std::quick_exit(1);
  });
}

// 安装全局处理器
void SetupGlobalHandlers() {
  // 日志的全局处理器，捕获所有未被捕获的异常，防止程序崩溃，并打印异常信息
  logging::InstallFallbackExceptionHandler();
  // 清理函数
  RegisterAtExitCleanup();
}
}  // namespace ncaf::internal

namespace ncaf {
void Init(uint32_t thread_pool_size) {
  internal::logging::Info("Initializing ncaf in single-node mode with default scheduler, thread_pool_size={}",
                          thread_pool_size);
  NCAF_THROW_CHECK(!internal::IsGlobalDefaultRegistryInitialized()) << "Already initialized.";
  // 创建全局默认注册表
  global_default_registry = std::make_unique<ActorRegistry>(thread_pool_size);
  // 安装全局处理器
  internal::SetupGlobalHandlers();
}

void Init(uint32_t thread_pool_size, uint32_t this_node_id, const std::vector<NodeInfo>& cluster_node_info) {
  internal::logging::Info(
      "Initializing ncaf in distributed mode with default scheduler, thread_pool_size={}, this_node_id={}, "
      "total_nodes={}",
      thread_pool_size, this_node_id, cluster_node_info.size());
  NCAF_THROW_CHECK(!internal::IsGlobalDefaultRegistryInitialized()) << "Already initialized.";
  global_default_registry = std::make_unique<ActorRegistry>(thread_pool_size, this_node_id, cluster_node_info);
  internal::SetupGlobalHandlers();
}

//
void HoldResource(std::shared_ptr<void> resource) { resource_holder.push_back(std::move(resource)); }

// 关键
void Shutdown() {
  internal::logging::Info("Shutting down ncaf.");
  NCAF_THROW_CHECK(internal::IsGlobalDefaultRegistryInitialized()) << "Not initialized.";
  // 重置全局默认注册表，销毁注册表实例，触发注册表的析构函数，清理资源
  global_default_registry.reset();
  // 清理资源，确保在全局默认注册表销毁前资源不会被销毁
  resource_holder.clear();
}

void ConfigureLogging(const logging::LogConfig& config) {
  internal::logging::GlobalLogger() = internal::logging::CreateLoggerUsingConfig(config);
}

}  // namespace ncaf