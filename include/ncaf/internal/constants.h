#include <chrono>
namespace ncaf {
namespace internal {

constexpr size_t kEmptyActorRefHashVal = 10086;
constexpr auto kDefaultHeartbeatTimeout = std::chrono::milliseconds(5000);
constexpr auto kDefaultHeartbeatInterval = std::chrono::milliseconds(500);

}  // namespace internal
}  // namespace ncaf
