#include "ncaf/api.h"

class PingWorker {
 public:
  explicit PingWorker(std::string name) : name_(std::move(name)) {}

  // Boilerplate 1, the Create method
  static PingWorker Create(std::string name) { return PingWorker(std::move(name)); }

  std::string Ping(const std::string& message) { return "ack from " + name_ + ", msg got: " + message; }

 private:
  std::string name_;
};

ncaf_remote(&PingWorker::Create, &PingWorker::Ping);

int main(int /*argc*/, char** argv) {
  uint32_t this_node_id = std::atoi(argv[1]);
  auto coroutine = [](uint32_t this_node_id) -> exec::task<void> {
    std::vector<ncaf::NodeInfo> cluster_node_info = {{.node_id = 0, .address = "tcp://127.0.0.1:5301"},
                                                     {.node_id = 1, .address = "tcp://127.0.0.1:5302"}};
    ncaf::Init(/*thread_pool_size=*/4, this_node_id, cluster_node_info);
    uint32_t remote_node_id = (this_node_id + 1) % cluster_node_info.size();
    auto ping_worker = co_await ncaf::Spawn<PingWorker, &PingWorker::Create>(
        ncaf::ActorConfig {.node_id = remote_node_id}, /*name=*/"Alice");
    // Loop forever
    while (true) {
      auto ping = ping_worker.Send<&PingWorker::Ping>("hello");
      std::ignore = co_await std::move(ping);
    }
  };
  stdexec::sync_wait(coroutine(this_node_id));
}
