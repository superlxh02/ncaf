#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include <stdexec/execution.hpp>

namespace ncaf {
struct ActorConfig {
  size_t max_message_executed_per_activation = 100;  // 每次激活actor时最多执行的消息数量
  uint32_t node_id = 0;                              // actor所在节点的id
  std::optional<std::string> actor_name;             // 仅用于日志输出，方便区分不同actor实例,注意不能重复

  size_t scheduler_index = 0;  // 调度器索引，默认为0
  uint32_t priority =
      UINT32_MAX;  // 使用优先级线程池的时候可以设置优先级，数值越小优先级越高，默认为UINT32_MAX，表示最低优先级
};

struct get_priority_t {
  constexpr uint32_t operator()(const auto& prop) const noexcept {
    if constexpr (requires { prop.query(get_priority_t {}); }) {
      return prop.query(get_priority_t {});
    } else {
      return UINT32_MAX;
    }
  }
  constexpr auto query(stdexec::forwarding_query_t) const noexcept -> bool { return true; }
};

struct get_scheduler_index_t {
  constexpr size_t operator()(const auto& prop) const noexcept {
    if constexpr (requires { prop.query(get_scheduler_index_t {}); }) {
      return prop.query(get_scheduler_index_t {});
    } else {
      return 0;
    }
  }
  constexpr auto query(stdexec::forwarding_query_t) const noexcept -> bool { return true; }
};

constexpr inline get_priority_t get_priority {};
constexpr inline get_scheduler_index_t get_scheduler_index {};
}  // namespace ncaf