#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "ncaf/internal/actor.h"
#include "ncaf/internal/logging.h"
#include "ncaf/internal/network.h"
#include "ncaf/internal/reflect.h"
#include "ncaf/internal/serialization.h"

namespace ncaf::internal {

// 封装了一个actor的引用
// 用于actor调用其他actor的方法，隐藏了远程调用和本地调用的细节，提供统一的Send接口
template <class UserClass>
class ActorRef {
 public:
  ActorRef() : is_empty_(true) {}

  ActorRef(uint32_t this_node_id, uint32_t node_id, uint64_t actor_id, TypeErasedActor* actor,
           network::MessageBroker* message_broker)
      : is_empty_(false),
        this_node_id_(this_node_id),
        node_id_(node_id),
        actor_id_(actor_id),
        type_erased_actor_(actor),
        message_broker_(message_broker) {}

  friend bool operator==(const ActorRef& lhs, const ActorRef& rhs) {
    if (lhs.is_empty_ && rhs.is_empty_) {
      return true;
    }
    return lhs.node_id_ == rhs.node_id_ && lhs.actor_id_ == rhs.actor_id_;
  }

  void SetLocalRuntimeInfo(uint32_t this_node_id, TypeErasedActor* actor, network::MessageBroker* message_broker) {
    this_node_id_ = this_node_id;
    type_erased_actor_ = actor;
    message_broker_ = message_broker;
  }

  friend rfl::Reflector<ActorRef<UserClass>>;

  template <auto kMethod, class... Args>
  [[nodiscard]] auto Send(Args... args) const {
    // Add a fallback inline_scheduler for it.
    return util::WrapSenderWithInlineScheduler(SendInternal<kMethod>(std::move(args)...));
  }

  // 给本地actor发送消息，性能比Send()更好，没有堆内存分配
  template <auto kMethod, class... Args>
  [[nodiscard]] ex::sender auto SendLocal(Args... args) const {
    static_assert(std::is_invocable_v<decltype(kMethod), UserClass*, Args...>,
                  "method is not invocable with the provided arguments");
    if (IsEmpty()) [[unlikely]] {
      throw std::runtime_error("Empty ActorRef, cannot call method on it.");
    }
    NCAF_THROW_CHECK_EQ(node_id_, this_node_id_) << "Cannot call remote actor using SendLocal, use Send instead.";
    return type_erased_actor_->template CallActorMethod<kMethod>(std::move(args)...);
  }

  bool IsEmpty() const { return is_empty_; }

  uint32_t GetNodeId() const { return node_id_; }
  uint64_t GetActorId() const { return actor_id_; }

 private:
  bool is_empty_;                                     // 是否是空引用
  uint32_t this_node_id_ = 0;                         // 当前节点id
  uint32_t node_id_ = 0;                              // 目标节点id
  uint64_t actor_id_ = 0;                             // 全局唯的一actor id
  TypeErasedActor* type_erased_actor_ = nullptr;      // 本地actor示例指针
  network::MessageBroker* message_broker_ = nullptr;  // 网络消息代理

  // 统-一的发送接口，根据是不是本地actor来决定调用SendLocal还是走远程调用逻辑
  template <auto kMethod, class... Args>
  [[nodiscard]] auto SendInternal(Args... args) const
      -> exec::task<typename decltype(reflect::UnwrapReturnSenderIfNested<kMethod>())::type> {
    static_assert(std::is_invocable_v<decltype(kMethod), UserClass*, Args...>,
                  "method is not invocable with the provided arguments");
    if (IsEmpty()) [[unlikely]] {
      throw std::runtime_error("Empty ActorRef, cannot call method on it.");
    }
    // 如果是本地actor，直接调用sendlocal
    if (node_id_ == this_node_id_) {
      co_return co_await SendLocal<kMethod>(std::move(args)...);
    }

    // 远程调用
    NCAF_THROW_CHECK(message_broker_ != nullptr) << "Message broker not set";
    using Sig = reflect::Signature<decltype(kMethod)>;
    // 获取到方法参数的类型，并构造对应方法结构体
    serde::ActorMethodCallArgs<typename Sig::DecayedArgsTupleType> method_call_args {
        .args_tuple = typename Sig::DecayedArgsTupleType(std::move(args)...)};
    // 序列化数据
    auto serialized_args = serde::Serialize(method_call_args);
    // 获取方法名
    std::string handler_key = reflect::GetUniqueNameForFunction<kMethod>();
    // 构造协议数据包,预留长度
    serde::BufferWriter buffer_writer(network::ByteBufferType {sizeof(serde::NetworkRequestType) + sizeof(uint64_t) +
                                                               sizeof(handler_key.size()) + handler_key.size() +
                                                               sizeof(actor_id_) + serialized_args.size()});
    // protocol: [request_type][handler_key_len][handler_key][actor_id][ActorMethodCallArgs]

    // 往buffer里写入协议数据
    buffer_writer.WritePrimitive(serde::NetworkRequestType::kActorMethodCallRequest);  // 写请求类型
    buffer_writer.WritePrimitive(handler_key.size());                                  // 写handler key长度
    buffer_writer.CopyFrom(handler_key.data(), handler_key.size());                    // 写handler key
    buffer_writer.WritePrimitive(actor_id_);                                           // 写actor id
    buffer_writer.CopyFrom(serialized_args.data(), serialized_args.size());            // 写方法参数

    // 获取方法返回值类型
    using UnwrappedType = decltype(reflect::UnwrapReturnSenderIfNested<kMethod>())::type;
    // 接受回应buffer
    network::ByteBufferType response_buffer =
        co_await message_broker_->SendRequest(node_id_, std::move(buffer_writer).MoveBufferOut());
    // 构建reader来解析回应buffer
    serde::BufferReader reader {std::move(response_buffer)};
    // 解析回应buffer，先读取回应类型
    auto type = reader.NextPrimitive<serde::NetworkReplyType>();
    // 如果回应类型是错误，解析错误信息并抛出异常
    if (type == serde::NetworkReplyType::kActorMethodCallError) {
      auto res = serde::Deserialize<serde::ActorMethodReturnError>(reader.Current(), reader.RemainingSize());
      NCAF_THROW << res.error;
    }
    if constexpr (std::is_void_v<UnwrappedType>) {
      co_return;
    } else {
      // 如果回应类型是成功，解析返回值并返回
      auto res =
          serde::Deserialize<serde::ActorMethodReturnValue<UnwrappedType>>(reader.Current(), reader.RemainingSize());
      co_return res.return_value;
    }
  }
};

}  // namespace ncaf::internal

namespace ncaf {
using internal::ActorRef;
}  // namespace ncaf

// 特化std::hash以支持ActorRef作为unordered_map的key
// 哈希计算方式：
// 1.分别对 ActorId 和 NodeId 计算哈希
// 2.用 异或（XOR） 合并两个哈希值
namespace std {
template <class UserClass>
struct hash<ncaf::ActorRef<UserClass>> {
  size_t operator()(const ncaf::ActorRef<UserClass>& ref) const {
    if (ref.IsEmpty()) {
      return ncaf::internal::kEmptyActorRefHashVal;
    }
    return std::hash<uint64_t>()(ref.GetActorId()) ^ std::hash<uint32_t>()(ref.GetNodeId());
  }
};
}  // namespace std
