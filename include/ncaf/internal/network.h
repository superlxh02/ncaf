#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <latch>
#include <thread>

#include <exec/async_scope.hpp>
#include <exec/task.hpp>
#include <zmq.hpp>
#include <zmq_addon.hpp>

#include "ncaf/internal/constants.h"
#include "ncaf/internal/util.h"

namespace ncaf {
// 节点信息结构体·
struct NodeInfo {
  uint32_t node_id = 0;
  std::string address;
};
}  // namespace ncaf

namespace ncaf::internal::network {

using ByteBufferType = zmq::message_t;

enum class MessageFlag : uint8_t { kNormal = 0, kQuit, kHeartbeat };

// 标识符结构体，包含请求节点id、响应节点id、请求id和消息标志
struct Identifier {
  uint32_t request_node_id;
  uint32_t response_node_id;
  uint64_t request_id_in_node;
  MessageFlag flag;
};

// 心跳配置结构体，包含心跳超时时间和心跳间隔时间
struct HeartbeatConfig {
  std::chrono::milliseconds heartbeat_timeout = kDefaultHeartbeatTimeout;
  std::chrono::milliseconds heartbeat_interval = kDefaultHeartbeatInterval;
};

// 消息代理类，负责节点间的消息传递和请求响应
class MessageBroker {
 public:
  explicit MessageBroker(std::vector<NodeInfo> node_list, uint32_t this_node_id,
                         std::function<void(uint64_t received_request_id, ByteBufferType data)> request_handler,
                         HeartbeatConfig heartbeat_config = {});
  ~MessageBroker();

  void ClusterAlignedStop();

  /*
    以下是 std::execution sender 的适配
  */

  struct TypeErasedSendOperation {
    virtual ~TypeErasedSendOperation() = default;
    virtual void Complete(ByteBufferType /*response_data*/) {
      NCAF_THROW << "TypeErasedOperation::Complete should not be called";
    }
    TypeErasedSendOperation(Identifier identifier, ByteBufferType data, MessageBroker* message_broker)
        : identifier(identifier), data(std::move(data)), message_broker(message_broker) {}
    Identifier identifier;
    ByteBufferType data;
    MessageBroker* message_broker {};
  };

  // 自定义OperationState，继承于TypeErasedSendOperation，封装了start接口和完成操作的接口
  template <ex::receiver R>
  struct SendRequestOperation : TypeErasedSendOperation {
    SendRequestOperation(Identifier identifier, ByteBufferType data, MessageBroker* message_broker, R receiver)
        : TypeErasedSendOperation(identifier, std::move(data), message_broker), receiver(std::move(receiver)) {}
    R receiver;
    std::atomic_bool started = false;
    //
    void start() noexcept {
      bool expected = false;
      bool changed = started.compare_exchange_strong(expected, true);
      if (!changed) [[unlikely]] {
        logging::Critical("MessageBroker Operation already started");
        std::terminate();
      }
      message_broker->PushOperation(this);
    }
    // 完成操作，设置响应数据
    void Complete(ByteBufferType response_data) override { receiver.set_value(std::move(response_data)); }
  };
  // 自定义sender，封装了connect接口
  struct SendRequestSender : ex::sender_t {
    using completion_signatures = ex::completion_signatures<ex::set_value_t(ByteBufferType)>;
    Identifier identifier;
    ByteBufferType data;
    MessageBroker* message_broker;
    template <ex::receiver R>
    SendRequestOperation<R> connect(R receiver) {
      return SendRequestOperation<R>(identifier, std::move(data), message_broker, std::move(receiver));
    }
  };

  /*
    以下是消息发送接口，SendRequest用于发送请求消息，ReplyRequest用于发送响应消息
  */

  SendRequestSender SendRequest(uint32_t to_node_id, ByteBufferType data, MessageFlag flag = MessageFlag::kNormal);

  void ReplyRequest(uint64_t received_request_id, ByteBufferType data);

 private:
  void EstablishConnections();
  void PushOperation(TypeErasedSendOperation* operation);
  void SendProcessLoop(const std::stop_token& stop_token);
  void ReceiveProcessLoop(const std::stop_token& stop_token);
  void HandleReceivedMessage(zmq::multipart_t multi);
  void CheckHeartbeat();
  void SendHeartbeat();

  struct ReplyOperation {
    Identifier identifier;
    ByteBufferType data;
  };

  std::vector<NodeInfo> node_list_;                                                         // 集群中所有节点的信息
  uint32_t this_node_id_;                                                                   // 当前节点的id
  std::function<void(uint64_t received_request_id, ByteBufferType data)> request_handler_;  // 收到请求消息后的处理函数
  HeartbeatConfig heartbeat_;                                                               // 心跳配置
  std::atomic_uint64_t send_request_id_counter_ = 0;  // 发送请求id计数器，每次发送请求消息时自增，
  std::atomic_uint64_t received_request_id_counter_ =
      0;  // 收到的请求id计数器，每次收到请求消息时自增，用于生成唯一的请求id

  zmq::context_t context_ {/*io_threads_=*/1};  // ZeroMQ上下文对象，负责管理ZeroMQ的资源和线程
  util::LockGuardedMap<uint32_t, zmq::socket_t>
      node_id_to_send_socket_;  // 发送消息的socket，每个节点一个，使用LockGuardedMap保护线程安全
  zmq::socket_t recv_socket_ {
      context_, zmq::socket_type::dealer};  // 接收消息的socket，使用dealer模式，可以同时接收多个节点的消息

  util::LinearizableUnboundedMpscQueue<TypeErasedSendOperation*>
      pending_send_operations_;  // 待发送的消息操作队列，使用LinearizableUnboundedMpscQueue保护线程安全
  util::LockGuardedMap<uint64_t, TypeErasedSendOperation*>
      send_request_id_to_operation_;  // 发送请求id到操作的映射，用于在收到响应消息时找到对应的操作，设置响应数据
  util::LinearizableUnboundedMpscQueue<ReplyOperation>
      pending_reply_operations_;  // 待发送的响应操作队列，使用LinearizableUnboundedMpscQueue保护线程安全
  util::LockGuardedMap<uint64_t, Identifier>
      received_request_id_to_identifier_;  // 收到的请求id到标识符的映射，用于在收到请求消息时记录请求信息，在发送响应消息时找到对应的请求信息

  std::jthread send_thread_;          // 发送线程
  std::jthread recv_thread_;          // 接收线程
  std::atomic_bool stopped_ = false;  // 是否已经停止，控制发送线程和接收线程的退出
  std::latch quit_latch_;             // 用于等待发送线程和接收线程退出
  exec::async_scope async_scope_;     // 异步作用域，用于管理异步任务的生命周期，确保在停止时等待所有异步任务完成

  using TimePoint = std::chrono::time_point<std::chrono::steady_clock>;  // 时间点类型
  TimePoint last_heartbeat_;                                             // 上次收到心跳的时间点
  std::unordered_map<uint32_t, TimePoint> last_seen_;  // 每个节点上次被看到的时间点，用于检测节点是否失联
};

}  // namespace ncaf::internal::network

namespace ncaf {
using internal::network::HeartbeatConfig;
}  // namespace ncaf
