#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "ncaf/internal/actor.h"
#include "ncaf/internal/actor_ref.h"
#include "ncaf/internal/logging.h"
#include "ncaf/internal/network.h"
#include "ncaf/internal/reflect.h"
#include "ncaf/internal/scheduler.h"
#include "ncaf/internal/serialization.h"
#include "ncaf/internal/util.h"

namespace ncaf::internal {

//
class ActorRegistryRequestProcessor {
 public:
  explicit ActorRegistryRequestProcessor(std::unique_ptr<TypeErasedActorScheduler> scheduler, uint32_t this_node_id,
                                         const std::vector<NodeInfo>& cluster_node_info,
                                         network::MessageBroker* message_broker);

  // 销毁所有actor，等待销毁完成
  exec::task<void> AsyncDestroyAllActors();

  // 默认配置创建一个actor，返回一个ActorRef
  template <class UserClass, auto kCreateFn = nullptr, class... Args>
  exec::task<ActorRef<UserClass>> CreateActor(Args... args) {
    return CreateActor<UserClass, kCreateFn>(ActorConfig {.node_id = this_node_id_}, std::move(args)...);
  }

  // 自定义配置创建一个actor，返回一个ActorRef
  template <class UserClass, auto kCreateFn = nullptr, class... Args>
  exec::task<ActorRef<UserClass>> CreateActor(ActorConfig config, Args... args) {
    if constexpr (kCreateFn != nullptr) {
      static_assert(std::is_invocable_v<decltype(kCreateFn), Args...>,
                    "Class can't be created by given args and create function");
    }
    if (is_distributed_mode_) {
      NCAF_THROW_CHECK(node_id_to_address_.contains(config.node_id)) << "Invalid node id: " << config.node_id;
    }

    // 如果是本地节点，直接使用make_unique在堆上创建一个actor，随机生成一个actor_id,构造一个ActorRef返回
    if (config.node_id == this_node_id_) {
      auto actor_id = GenerateRandomActorId();
      auto actor = std::make_unique<Actor<UserClass, kCreateFn>>(scheduler_->Clone(), config, std::move(args)...);
      auto handle = ActorRef<UserClass>(this_node_id_, config.node_id, actor_id, actor.get(), message_broker_);
      if (config.actor_name.has_value()) {
        std::string& name = *config.actor_name;
        NCAF_THROW_CHECK(!actor_name_to_id_.contains(name))
            << "An actor with the same name already exists, name=" << name;
        actor_name_to_id_[name] = actor_id;
      }
      actor_id_to_actor_[actor_id] = std::move(actor);
      co_return handle;
    }

    if constexpr (kCreateFn == nullptr) {
      NCAF_THROW << "CreateActor<UserClass> can only be used to create local actor, to create remote actor, use "
                    "CreateActor<UserClass, kCreateFn> to provide a fixed signature for remote actor creation. node_id="
                 << config.node_id << ", this_node_id=" << this_node_id_ << ", actor_type=" << typeid(UserClass).name();
    }

    // 如果是远程节点，构造一个协议数据包，发送给远程节点，等待远程节点创建完成后返回的actor id，构造一个ActorRef返回
    if constexpr (kCreateFn != nullptr) {
      using CreateFnSig = reflect::Signature<decltype(kCreateFn)>;

      // protocol: [message_type][handler_key_len][handler_key][ActorCreationArgs]
      typename CreateFnSig::DecayedArgsTupleType args_tuple {std::move(args)...};
      // 构造ActorCreationArgs结构体，包含actor配置和创建函数参数
      serde::ActorCreationArgs<typename CreateFnSig::DecayedArgsTupleType> actor_creation_args {config,

                                                                                                std::move(args_tuple)};
      // 序列化ActorCreationArgs结构体，得到一个字节数组
      std::vector<char> serialized = serde::Serialize(actor_creation_args);

      // 获取创建函数的唯一标识符，作为handler key
      std::string handler_key = reflect::GetUniqueNameForFunction<kCreateFn>();
      // 构造协议数据包，预留长度
      serde::BufferWriter buffer_writer(network::ByteBufferType {
          serialized.size() + sizeof(uint64_t) + handler_key.size() + sizeof(serde::NetworkRequestType)});
      // 填充协议数据包
      buffer_writer.WritePrimitive(serde::NetworkRequestType::kActorCreationRequest);
      buffer_writer.WritePrimitive(handler_key.size());
      buffer_writer.CopyFrom(handler_key.data(), handler_key.size());
      buffer_writer.CopyFrom(serialized.data(), serialized.size());
      NCAF_THROW_CHECK(buffer_writer.EndReached()) << "Buffer writer not ended";

      // 发给远程
      auto response_buffer =
          co_await message_broker_->SendRequest(config.node_id, std::move(buffer_writer).MoveBufferOut());
      // 解析回应buffer，先读取回应类型，如果是错误，解析错误信息并抛出异常，如果是成功，解析actor
      // id，构造一个ActorRef返回
      serde::BufferReader reader(std::move(response_buffer));
      auto type = reader.template NextPrimitive<serde::NetworkReplyType>();
      if (type == serde::NetworkReplyType::kActorCreationError) {
        NCAF_THROW << "Got actor creation error from remote node:"
                   << serde::Deserialize<serde::ActorCreationError>(reader.Current(), reader.RemainingSize()).error;
      }
      auto actor_id = reader.template NextPrimitive<uint64_t>();
      // 构造一个ActorRef返回
      co_return ActorRef<UserClass>(this_node_id_, config.node_id, actor_id, nullptr, message_broker_);
    }
  }

  // 销毁某一个actor
  template <class UserClass>
  exec::task<void> DestroyActor(const ActorRef<UserClass>& actor_ref) {
    auto actor_id = actor_ref.GetActorId();
    NCAF_THROW_CHECK(actor_id_to_actor_.contains(actor_id)) << "Actor with id " << actor_id << " not found";
    auto actor = std::move(actor_id_to_actor_.at(actor_id));
    actor_id_to_actor_.erase(actor_id);

    auto actor_name = actor->GetActorConfig().actor_name;
    if (actor_name.has_value()) {
      actor_name_to_id_.erase(actor_name.value());
    }
    co_await actor->AsyncDestroy();
  }

  // 根据名字查找actor，如果是本地节点，直接在actor_name_to_id_里查找，如果是远程节点，发送一个ActorLookUpRequest给远程节点，等待回应
  template <class UserClass>
  std::optional<ActorRef<UserClass>> GetActorRefByName(const std::string& name) const {
    if (actor_name_to_id_.contains(name)) {
      const auto actor_id = actor_name_to_id_.at(name);
      const auto& actor = actor_id_to_actor_.at(actor_id);
      return ActorRef<UserClass>(this_node_id_, this_node_id_, actor_id, actor.get(), message_broker_);
    }
    return std::nullopt;
  }

  template <class UserClass>
  exec::task<std::optional<ActorRef<UserClass>>> GetActorRefByName(const uint32_t& node_id,
                                                                   const std::string& name) const {
    if (node_id == this_node_id_) {
      co_return GetActorRefByName<UserClass>(name);
    }

    std::vector<char> serialized = serde::Serialize(serde::ActorLookUpRequest {name});
    serde::BufferWriter<network::ByteBufferType> writer(
        network::ByteBufferType(sizeof(serde::NetworkRequestType) + serialized.size()));
    writer.WritePrimitive(serde::NetworkRequestType::kActorLookUpRequest);
    writer.CopyFrom(serialized.data(), serialized.size());

    auto response_buffer =
        co_await message_broker_->SendRequest(node_id, network::ByteBufferType {std::move(writer).MoveBufferOut()});

    std::optional<uint64_t> actor_id = std::nullopt;
    serde::BufferReader<network::ByteBufferType> reader(std::move(response_buffer));
    auto type = reader.NextPrimitive<serde::NetworkReplyType>();
    if (type == serde::NetworkReplyType::kActorLookUpReturn) {
      actor_id = reader.NextPrimitive<uint64_t>();
    }

    if (actor_id.has_value()) {
      co_return ActorRef<UserClass>(this_node_id_, node_id, actor_id.value(), nullptr, message_broker_);
    }

    co_return std::nullopt;
  }

  // 处理网络请求
  exec::task<void> HandleNetworkRequest(uint64_t received_request_id, network::ByteBufferType request_buffer);

 private:
  bool is_distributed_mode_ = false;                                                  // 是否是分布式模式
  std::mt19937 random_num_generator_;                                                 // 随机数生成器
  std::unique_ptr<TypeErasedActorScheduler> scheduler_;                               // 调度器
  uint32_t this_node_id_ = 0;                                                         // 节点id
  std::unordered_map<uint32_t, std::string> node_id_to_address_;                      // 节点id到地址的映射表
  network::MessageBroker* message_broker_ = nullptr;                                  // 网络消息代理
  std::unordered_map<uint64_t, std::unique_ptr<TypeErasedActor>> actor_id_to_actor_;  // actor id到actor实例的映射表
  std::unordered_map<std::string, std::uint64_t> actor_name_to_id_;                   // actor name到actor id的映射表

  // 初始化，使用随机设备生成一个随机数种子，初始化随机数生成器
  void InitRandomNumGenerator();
  // 生成一个随机的actor id
  uint64_t GenerateRandomActorId();
  // 验证节点信息的合法性，检查是否有重复的节点id
  void ValidateNodeInfo(const std::vector<NodeInfo>& cluster_node_info);
  // 解析消息类型，检查消息长度是否合法，返回消息类型
  serde::NetworkRequestType ParseMessageType(const network::ByteBufferType& buffer);
  // 回复错误消息，构造一个协议数据包，发送给远程节点
  void ReplyError(uint64_t received_request_id, serde::NetworkReplyType reply_type, std::string error_msg);
  // 处理actor创建请求，解析请求buffer，调用对应的handler创建actor，回复创建结果
  void HandleActorCreationRequest(uint64_t received_request_id, serde::BufferReader<network::ByteBufferType> reader);
  // 处理actor方法调用请求，解析请求buffer，找到对应的handler，调用handler处理请求，等待handler返回结果，回复结果
  exec::task<void> HandleActorMethodCallRequest(uint64_t received_request_id,
                                                serde::BufferReader<network::ByteBufferType> reader);
};

class ActorRegistry {
 public:
  // 只传线程池大小，构造一个默认的工作共享线程池作为调度器，单节点模式
  explicit ActorRegistry(uint32_t thread_pool_size)
      : ActorRegistry(thread_pool_size, /*scheduler=*/nullptr, /*this_node_id=*/0, /*cluster_node_info=*/ {},
                      /*heartbeat_config=*/ {}) {}

  // 只传调度器，单节点模式
  template <ex::scheduler Scheduler>
  explicit ActorRegistry(Scheduler scheduler)
      : ActorRegistry(/*thread_pool_size=*/0, std::make_unique<AnyStdExecScheduler<Scheduler>>(scheduler),
                      /*this_node_id=*/0, /*cluster_node_info=*/ {}, /*heartbeat_config=*/ {}) {}

  /**
   * @brief Construct in distributed mode, use the default work-sharing thread pool as the scheduler.
   */

  // 传线程池大小，节点id和集群信息，构造一个默认的工作共享线程池作为调度器，分布式模式
  explicit ActorRegistry(uint32_t thread_pool_size, uint32_t this_node_id,
                         const std::vector<NodeInfo>& cluster_node_info, network::HeartbeatConfig heartbeat_config = {})
      : ActorRegistry(thread_pool_size, /*scheduler=*/nullptr, this_node_id, cluster_node_info, heartbeat_config) {}

  // 传调度器，节点id和集群信息，构造一个指定的调度器，分布式模式
  template <ex::scheduler Scheduler>
  explicit ActorRegistry(Scheduler scheduler, uint32_t this_node_id, const std::vector<NodeInfo>& cluster_node_info,
                         network::HeartbeatConfig heartbeat_config = {})
      : ActorRegistry(/*thread_pool_size=*/0, std::make_unique<AnyStdExecScheduler<Scheduler>>(scheduler), this_node_id,
                      cluster_node_info, heartbeat_config) {}

  ~ActorRegistry();

  // 创建actor，使用默认配置，返回一个ActorRef
  template <class UserClass, auto kCreateFn = nullptr, class... Args>
  reflect::AwaitableOf<ActorRef<UserClass>> auto CreateActor(Args... args) {
    // resolve overload ambiguity
    constexpr exec::task<ActorRef<UserClass>> (ActorRegistryRequestProcessor::*kProcessFn)(Args...) =
        &ActorRegistryRequestProcessor::CreateActor<UserClass, kCreateFn, Args...>;

    return util::WrapSenderWithInlineScheduler(processor_actor_ref_.SendLocal<kProcessFn>(std::move(args)...));
  }

  // 创建actor，使用自定义配置，返回一个ActorRef
  template <class UserClass, auto kCreateFn = nullptr, class... Args>
  reflect::AwaitableOf<ActorRef<UserClass>> auto CreateActor(ActorConfig config, Args... args) {
    // resolve overload ambiguity
    constexpr exec::task<ActorRef<UserClass>> (ActorRegistryRequestProcessor::*kProcessFn)(ActorConfig, Args...) =
        &ActorRegistryRequestProcessor::CreateActor<UserClass, kCreateFn, Args...>;

    return util::WrapSenderWithInlineScheduler(processor_actor_ref_.SendLocal<kProcessFn>(config, std::move(args)...));
  }

  // 销毁actor
  template <class UserClass>
  reflect::AwaitableOf<void> auto DestroyActor(const ActorRef<UserClass>& actor_ref) {
    return util::WrapSenderWithInlineScheduler(
        processor_actor_ref_.SendLocal<&ActorRegistryRequestProcessor::DestroyActor<UserClass>>(actor_ref));
  }

  // 根据名字查找actor
  template <class UserClass>
  reflect::AwaitableOf<std::optional<ActorRef<UserClass>>> auto GetActorRefByName(const std::string& name) const {
    // resolve overload ambiguity
    constexpr std::optional<ActorRef<UserClass>> (ActorRegistryRequestProcessor::*kProcessFn)(const std::string& name)
        const = &ActorRegistryRequestProcessor::GetActorRefByName<UserClass>;

    return util::WrapSenderWithInlineScheduler(processor_actor_ref_.SendLocal<kProcessFn>(name));
  }

  template <class UserClass>
  reflect::AwaitableOf<std::optional<ActorRef<UserClass>>> auto GetActorRefByName(const uint32_t& node_id,
                                                                                  const std::string& name) const {
    // resolve overload ambiguity
    constexpr exec::task<std::optional<ActorRef<UserClass>>> (ActorRegistryRequestProcessor::*kProcessFn)(
        const uint32_t& node_id, const std::string& name) const =
        &ActorRegistryRequestProcessor::GetActorRefByName<UserClass>;

    return util::WrapSenderWithInlineScheduler(processor_actor_ref_.SendLocal<kProcessFn>(node_id, name));
  }

 private:
  bool is_distributed_mode_;                                     // 是否是分布式模式
  uint32_t this_node_id_;                                        // 当前节点id
  WorkSharingThreadPool default_work_sharing_thread_pool_;       // 默认的工作共享线程池
  std::unique_ptr<TypeErasedActorScheduler> scheduler_;          // 调度器
  std::unique_ptr<network::MessageBroker> message_broker_;       // 网络消息代理
  Actor<ActorRegistryRequestProcessor> processor_actor_;         // 处理请求的actor实例
  ActorRef<ActorRegistryRequestProcessor> processor_actor_ref_;  // 处理请求的actor的引用
  exec::async_scope async_scope_;  // 用于管理异步任务的生命周期，确保在ActorRegistry销毁时所有异步任务都已完成

  explicit ActorRegistry(uint32_t thread_pool_size, std::unique_ptr<TypeErasedActorScheduler> scheduler,
                         uint32_t this_node_id, const std::vector<NodeInfo>& cluster_node_info,
                         network::HeartbeatConfig heartbeat_config = {});
};
}  // namespace ncaf::internal

// ----------------Global Default Registry--------------------------

namespace ncaf::internal {
ActorRegistry& GetGlobalDefaultRegistry();
void AssignGlobalDefaultRegistry(std::unique_ptr<ActorRegistry> registry);
bool IsGlobalDefaultRegistryInitialized();
void SetupGlobalHandlers();
}  // namespace ncaf::internal

namespace ncaf {
using ncaf::internal::ActorRegistry;

// 只传线程池大小，构造一个默认的工作共享线程池作为调度器，单节点模式
void Init(uint32_t thread_pool_size);

// 传调度器，构造一个指定的调度器，单节点模式
template <ex::scheduler Scheduler>
void Init(Scheduler scheduler);

// 传线程池大小，节点id和集群信息，构造一个默认的工作共享线程池作为调度器，分布式模式
void Init(uint32_t thread_pool_size, uint32_t this_node_id, const std::vector<NodeInfo>& cluster_node_info);

// 传调度器，节点id和集群信息，构造一个指定的调度器，分布式模式
template <ex::scheduler Scheduler>
void Init(Scheduler scheduler, uint32_t this_node_id, const std::vector<NodeInfo>& cluster_node_info);

// 用于持有资源，确保在全局默认注册表销毁前资源不会被销毁
void HoldResource(std::shared_ptr<void> resource);

// 销毁函数，用户需要显示调用
void Shutdown();

// 创建actor，使用默认配置，返回一个ActorRef
template <class UserClass, auto kCreateFn = nullptr, class... Args>
internal::reflect::AwaitableOf<ActorRef<UserClass>> auto Spawn(Args... args) {
  return internal::GetGlobalDefaultRegistry().CreateActor<UserClass, kCreateFn, Args...>(std::move(args)...);
}

// 创建actor，使用自定义配置，返回一个ActorRef
template <class UserClass, auto kCreateFn = nullptr, class... Args>
internal::reflect::AwaitableOf<ActorRef<UserClass>> auto Spawn(ActorConfig config, Args... args) {
  return internal::GetGlobalDefaultRegistry().CreateActor<UserClass, kCreateFn, Args...>(config, std::move(args)...);
}

// 销毁actor
template <class UserClass>
internal::reflect::AwaitableOf<void> auto DestroyActor(const ActorRef<UserClass>& actor_ref) {
  return internal::GetGlobalDefaultRegistry().DestroyActor<UserClass>(actor_ref);
}

// 根据名字查找actor
template <class UserClass>
internal::reflect::AwaitableOf<std::optional<ActorRef<UserClass>>> auto GetActorRefByName(const std::string& name) {
  return internal::GetGlobalDefaultRegistry().GetActorRefByName<UserClass>(name);
}

// 根据名字查找actor，
template <class UserClass>
internal::reflect::AwaitableOf<std::optional<ActorRef<UserClass>>> auto GetActorRefByName(const uint32_t& node_id,
                                                                                          const std::string& name) {
  return internal::GetGlobalDefaultRegistry().GetActorRefByName<UserClass>(node_id, name);
}

void ConfigureLogging(const logging::LogConfig& config = {});
}  // namespace ncaf

// -----------template function implementations-------------

namespace ncaf {
template <ex::scheduler Scheduler>
void Init(Scheduler scheduler) {
  internal::logging::Info("Initializing ncaf in single-node mode with custom scheduler.");
  NCAF_THROW_CHECK(!internal::IsGlobalDefaultRegistryInitialized()) << "Already initialized.";
  AssignGlobalDefaultRegistry(std::make_unique<ActorRegistry>(std::move(scheduler)));
  internal::SetupGlobalHandlers();
}

template <ex::scheduler Scheduler>
void Init(Scheduler scheduler, uint32_t this_node_id, const std::vector<NodeInfo>& cluster_node_info) {
  internal::logging::Info(
      "Initializing ncaf in distributed mode with custom scheduler. this_node_id={}, total_nodes={}", this_node_id,
      cluster_node_info.size());
  NCAF_THROW_CHECK(!internal::IsGlobalDefaultRegistryInitialized()) << "Already initialized.";
  AssignGlobalDefaultRegistry(std::make_unique<ActorRegistry>(std::move(scheduler), this_node_id, cluster_node_info));
  internal::SetupGlobalHandlers();
}
}  // namespace ncaf