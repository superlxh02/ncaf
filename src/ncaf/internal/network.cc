#include "ncaf/internal/network.h"

#include <atomic>
#include <chrono>
#include <coroutine>
#include <functional>
#include <thread>
#include <utility>

#include <exec/async_scope.hpp>
#include <spdlog/spdlog.h>

#include "ncaf/internal/logging.h"
#include "ncaf/internal/serialization.h"

namespace ncaf::internal::network {

MessageBroker::MessageBroker(std::vector<ncaf::NodeInfo> node_list, uint32_t this_node_id,
                             std::function<void(uint64_t received_request_id, ByteBufferType data)> request_handler,
                             HeartbeatConfig heartbeat_config)
    : node_list_(std::move(node_list)),
      this_node_id_(this_node_id),
      request_handler_(std::move(request_handler)),
      heartbeat_(heartbeat_config),
      quit_latch_(node_list_.size()),
      last_heartbeat_(std::chrono::steady_clock::now()) {
  EstablishConnections();

  auto start_time_point = std::chrono::steady_clock::now();
  for (const auto& node : node_list_) {
    if (node.node_id != this_node_id_) {
      last_seen_.emplace(node.node_id, start_time_point);
    }
  }

  send_thread_ = std::jthread([this](const std::stop_token& stop_token) { SendProcessLoop(stop_token); });
  recv_thread_ = std::jthread([this](const std::stop_token& stop_token) { ReceiveProcessLoop(stop_token); });
}

MessageBroker::~MessageBroker() {
  if (!stopped_.load()) {
    ClusterAlignedStop();
  }
}

// 等待所有节点退出，回收所有线程
void MessageBroker::ClusterAlignedStop() {
  logging::Info("[Cluster Aligned Stop] Node {} sending quit message to all other nodes", this_node_id_);
  for (const auto& node : node_list_) {
    if (node.node_id != this_node_id_) {
      // 构造一个sender,发送一个请求消息，消息标志为kQuit，表示这是一个退出消息，数据为空，发送完成后不关心响应结果
      auto sender = SendRequest(node.node_id, ByteBufferType {}, MessageFlag::kQuit) | ex::then([](auto empty) {});
      async_scope_.spawn(std::move(sender));
    }
  }

  // latch数量减一，表示本节点已经发送完退出消息，等待其他节点也发送完退出消息
  quit_latch_.count_down();
  // 等待其他所有节点全部退出
  quit_latch_.wait();
  stopped_.store(true);
  ex::sync_wait(async_scope_.on_empty());
  logging::Info("[Cluster Aligned Stop] All nodes are going to quit, stopping node {}'s io threads.", this_node_id_);
  // 线程协作停止
  send_thread_.request_stop();
  recv_thread_.request_stop();
  send_thread_.join();
  recv_thread_.join();
  logging::Info("[Cluster Aligned Stop] Node {}'s io threads stopped, cluster aligned stop completed", this_node_id_);
}

// 建立连接
void MessageBroker::EstablishConnections() {
  // 遍历节点列表，找到本节点的地址并绑定recv_socket
  bool found_local_address = false;
  for (const auto& node : node_list_) {
    if (node.node_id == this_node_id_) {
      recv_socket_.bind(node.address);
      recv_socket_.set(zmq::sockopt::linger, 0);
      found_local_address = true;
      logging::Info("Node {}'s recv socket bound to {}", this_node_id_, node.address);
      break;
    }
  }

  NCAF_THROW_CHECK(found_local_address) << "Local address not found in node list, this_node_id: " << this_node_id_;

  // 遍历节点列表，找到其他节点的地址，创建send_socket,保存到发送socket的map中，key是节点id，value是socket对象
  // 连接到其他节点的地址
  for (const auto& node : node_list_) {
    if (node.node_id != this_node_id_) {
      bool inserted = node_id_to_send_socket_.Insert(node.node_id, zmq::socket_t(context_, zmq::socket_type::dealer));
      NCAF_THROW_CHECK(inserted) << "Node " << node.node_id << " already has a send socket";
      auto& send_socket = node_id_to_send_socket_.At(node.node_id);
      send_socket.set(zmq::sockopt::linger, 0);
      send_socket.connect(node.address);
      logging::Info("Node {} added a send socket, connected to node {} at {}", this_node_id_, node.node_id,
                    node.address);
    }
  }
}

// 发送请求，返一个sender
MessageBroker::SendRequestSender MessageBroker::SendRequest(uint32_t to_node_id, ByteBufferType data,
                                                            MessageFlag flag) {
  NCAF_THROW_CHECK_NE(to_node_id, this_node_id_) << "Cannot send message to current node";
  // 构造标识符结构体
  Identifier identifier {
      .request_node_id = this_node_id_,
      .response_node_id = to_node_id,
      .request_id_in_node = send_request_id_counter_.fetch_add(1),
      .flag = flag,
  };
  // 返回一个发送sender
  return SendRequestSender {
      .identifier = identifier,
      .data = std::move(data),
      .message_broker = this,
  };
}

// 回复请求
void MessageBroker::ReplyRequest(uint64_t received_request_id, ByteBufferType data) {
  // 获取该id对应的请求标识符
  auto identifier = received_request_id_to_identifier_.At(received_request_id);
  // 从map中删除该id对应的请求标识符
  received_request_id_to_identifier_.Erase(received_request_id);
  // 构造一个回复操作，包含响应数据和请求标识符
  pending_reply_operations_.Push(ReplyOperation {
      .identifier = identifier,
      .data = std::move(data),
  });
}

// 发操作，推到待发送队列中
void MessageBroker::PushOperation(TypeErasedSendOperation* operation) {
  send_request_id_to_operation_.Insert(operation->identifier.request_id_in_node, operation);
  pending_send_operations_.Push(operation);
}
// 循环发送
void MessageBroker::SendProcessLoop(const std::stop_token& stop_token) {
  util::SetThreadName("snd_proc_loop");
  while (!stop_token.stop_requested()) {
    bool any_item_pulled = false;
    // 循环弹出待发送的消息操作，直到队列为空
    while (auto optional_operation = pending_send_operations_.TryPop()) {
      // 获取一个操作
      auto* operation = optional_operation.value();
      // 先序列化操作的标识符，然后构造一个multipart消息，第一部分是标识符，第二部分数据
      auto serialized_identifier = internal::serde::Serialize(operation->identifier);
      zmq::multipart_t multi;
      // 往multi里写入标识符和长度
      multi.addmem(serialized_identifier.data(), serialized_identifier.size());
      // 往multi里写入opration携带的数据
      multi.add(std::move(operation->data));
      // 根据操作的标识符中的目标节点id，从发送socket的map中找到对应的socket，发送消息
      auto& send_socket = node_id_to_send_socket_.At(operation->identifier.response_node_id);
      // 更新最后一次发送心跳的时间
      last_heartbeat_ = std::chrono::steady_clock::now();
      // 发送消息
      NCAF_THROW_CHECK(multi.send(send_socket));
      // 如果操作的标识符的消息标志是kQuit或者kHeartbeat，表示这是一个退出消息或者心跳消息，不需要等待响应，直接完成操作
      if (operation->identifier.flag == MessageFlag::kQuit || operation->identifier.flag == MessageFlag::kHeartbeat) {
        // quit operation and heartbeat has no response, complete it immediately
        operation->Complete(ByteBufferType {});
      }
      any_item_pulled = true;
    }
    // 循环弹出待发送的响应操作，直到队列为空
    while (auto optional_reply_operation = pending_reply_operations_.TryPop()) {
      auto& reply_operation = optional_reply_operation.value();
      auto& send_socket = node_id_to_send_socket_.At(reply_operation.identifier.request_node_id);
      auto serialized_identifier = internal::serde::Serialize(reply_operation.identifier);
      zmq::multipart_t multi;
      multi.addmem(serialized_identifier.data(), serialized_identifier.size());
      multi.add(std::move(reply_operation.data));
      last_heartbeat_ = std::chrono::steady_clock::now();
      NCAF_THROW_CHECK(multi.send(send_socket));
      any_item_pulled = true;
    }
    SendHeartbeat();
    if (!any_item_pulled) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
}

void MessageBroker::ReceiveProcessLoop(const std::stop_token& stop_token) {
  util::SetThreadName("recv_proc_loop");
  recv_socket_.set(zmq::sockopt::rcvtimeo, 100);

  while (!stop_token.stop_requested()) {
    CheckHeartbeat();
    zmq::multipart_t multi;
    if (!multi.recv(recv_socket_)) {
      continue;
    }
    HandleReceivedMessage(std::move(multi));
  }
}

void MessageBroker::HandleReceivedMessage(zmq::multipart_t multi) {
  NCAF_THROW_CHECK_EQ(multi.size(), 2) << "Expected 2-part message, got " << multi.size() << " parts";

  // 分成两部分，第一部分是标识符，第二部分是数据
  zmq::message_t identifier_bytes = multi.pop();
  zmq::message_t data_bytes = multi.pop();

  // 解析标识符，得到一个Identifier结构体
  auto identifier = internal::serde::Deserialize<Identifier>(identifier_bytes.data<uint8_t>(), identifier_bytes.size());
  // 更新最后一次看到该节点的时间
  last_seen_[identifier.request_node_id] = std::chrono::steady_clock::now();

  // 如果消息标志是kQuit，表示这是一个退出消息，记录日志，latch数量减一，等待其他节点也发送完退出消息
  if (identifier.flag == MessageFlag::kQuit) {
    NCAF_THROW_CHECK_EQ(data_bytes.size(), 0) << "Quit message should not have data";
    logging::Info("[Cluster Aligned Stop] Node {} is going to quit", identifier.request_node_id);
    quit_latch_.count_down();
    return;
  }
  // 如果消息标志是kHeartbeat，表示这是一个心跳消息，不需要处理，直接返回
  if (identifier.flag == MessageFlag::kHeartbeat) {
    return;
  }

  // 根据标识符中的请求节点id和响应节点id，判断这是一个请求消息还是响应消息
  if (identifier.request_node_id == this_node_id_) {
    // 如果请求节点id是本节点，表示这是一个响应消息，从发送请求队列里移除
    TypeErasedSendOperation* operation = send_request_id_to_operation_.At(identifier.request_id_in_node);
    send_request_id_to_operation_.Erase(identifier.request_id_in_node);
    operation->Complete(std::move(data_bytes));
  } else if (identifier.response_node_id == this_node_id_) {
    // 如果响应节点id是本节点，表示这是一个请求消息，构造一个请求标识符，推到待处理请求队列里，等待处理线程处理
    auto received_request_id = received_request_id_counter_.fetch_add(1);
    received_request_id_to_identifier_.Insert(received_request_id, identifier);
    request_handler_(received_request_id, std::move(data_bytes));
  } else {
    NCAF_THROW << "Invalid identifier, " << NCAF_DUMP_VARS(identifier);
  }
}

// 检查心跳，检测节点是否失联
void MessageBroker::CheckHeartbeat() {
  for (const auto& node : node_list_) {
    if (node.node_id != this_node_id_ &&
        std::chrono::steady_clock::now() - last_seen_[node.node_id] >= heartbeat_.heartbeat_timeout) {
      logging::Error("Node {} detect that node {} is dead, try to exit", this_node_id_, node.node_id);
      // don't call static variables' destructors, or the program will hang in MessageBroker's destructor
      std::quick_exit(1);
    }
  }
}

// 发送心跳
void MessageBroker::SendHeartbeat() {
  if (!stopped_.load() && std::chrono::steady_clock::now() - last_heartbeat_ >= heartbeat_.heartbeat_interval) {
    for (const auto& node : node_list_) {
      if (node.node_id != this_node_id_) {
        auto heartbeat =
            SendRequest(node.node_id, ByteBufferType {}, MessageFlag::kHeartbeat) | ex::then([](auto&& null) {});
        last_heartbeat_ = std::chrono::steady_clock::now();
        async_scope_.spawn(std::move(heartbeat));
      }
    }
  }
}

}  // namespace ncaf::internal::network
