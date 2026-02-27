#pragma once

#include "ncaf/internal/actor.h"
#include "ncaf/internal/network.h"

namespace ncaf::internal {
// ActorRef反序列化信息结构体，包含反序列化ActorRef所需的信息
struct ActorRefDeserializationInfo {
  uint32_t this_node_id = 0;                                   // 当前节点id
  std::function<TypeErasedActor*(uint64_t)> actor_look_up_fn;  // 根据actor_id查找本地actor实例的函数，参数是actor_id
  network::MessageBroker* message_broker = nullptr;            // 消息代理指针，用于远程调用时发送消息
};
}  // namespace ncaf::internal
