#include <cassert>
#include <iostream>

#include "ncaf/api.h"

class Counter {
 public:
  int Add(int x) { return count_ += x; }

 private:
  int count_ = 0;
};

exec::task<void> TestBasicUseCase() {
  ncaf::WorkSharingThreadPool thread_pool(10);
  ncaf::ActorRegistry registry(thread_pool.GetScheduler());
  ncaf::ActorRef counter = co_await registry.CreateActor<Counter>();

  // Coroutine support!
  std::cout << co_await counter.Send<&Counter::Add>(1) << '\n';
  assert(co_await counter.Send<&Counter::Add>(1) == 2);
}

int main() { stdexec::sync_wait(TestBasicUseCase()); }