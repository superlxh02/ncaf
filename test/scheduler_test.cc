#include <atomic>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "ncaf/api.h"
#include "ncaf/internal/actor_config.h"

namespace ex = ncaf::ex;

TEST(SchedulerTest, TaskInWorkSharingThreadPoolShouldBeStoppable) {
  ncaf::WorkSharingThreadPool thread_pool(1);
  auto scheduler = thread_pool.GetScheduler();
  auto task = ex::schedule(scheduler) | ex::then([]() { std::this_thread::sleep_for(std::chrono::milliseconds(100)); });
  exec::async_scope scope;
  for (int i = 0; i < 100000; ++i) {
    scope.spawn(task);
  }
  scope.request_stop();
  ex::sync_wait(scope.on_empty());
}

struct TestActor {
  void Foo() {
    count++;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  int count = 0;
};
TEST(SchedulerTest, ActorTaskShouldBeStoppable) {
  ncaf::WorkSharingThreadPool thread_pool(1);
  ncaf::ActorRegistry registry(thread_pool.GetScheduler());
  auto [actor] = stdexec::sync_wait(registry.CreateActor<TestActor>()).value();
  exec::async_scope scope;
  for (int i = 0; i < 100000; ++i) {
    scope.spawn(actor.Send<&TestActor::Foo>());
  }
  scope.request_stop();
  ex::sync_wait(scope.on_empty());
}

TEST(SchedulerTest, PrioritySchedulerTest) {
  ncaf::PriorityThreadPool thread_pool(1, /*start_workers_immediately=*/false);
  auto scheduler = thread_pool.GetScheduler();
  std::atomic_int count = 0;
  auto sender1 = ex::schedule(scheduler) | ex::then([&count]() {
                   EXPECT_EQ(count, 0);
                   count++;
                 }) |
                 ex::write_env(ex::prop {ncaf::get_priority, 1});
  auto sender2 = ex::schedule(scheduler) | ex::then([&count]() {
                   EXPECT_EQ(count, 2);
                   count++;
                 }) |
                 ex::write_env(ex::prop {ncaf::get_priority, 3});
  auto sender3 = ex::schedule(scheduler) | ex::then([&count]() {
                   EXPECT_EQ(count, 1);
                   count++;
                 }) |
                 ex::write_env(ex::prop {ncaf::get_priority, 2});
  exec::async_scope scope;
  scope.spawn(sender1);
  scope.spawn(sender2);
  scope.spawn(sender3);
  thread_pool.StartWorkers();
  ex::sync_wait(scope.on_empty());
  ASSERT_EQ(count, 3);
}

struct TestActor2 {
  uint64_t GetThreadId() { return std::hash<std::thread::id> {}(std::this_thread::get_id()); }
};

TEST(SchedulerTest, SchedulerUnionTest) {
  ncaf::WorkSharingThreadPool thread_pool1(1);
  ncaf::WorkSharingThreadPool thread_pool2(1);
  ncaf::SchedulerUnion scheduler_union(
      std::vector<ncaf::WorkSharingThreadPool::Scheduler> {thread_pool1.GetScheduler(), thread_pool2.GetScheduler()});
  auto scheduler = scheduler_union.GetScheduler();
  auto start = ex::schedule(scheduler) | ex::then([] { return std::this_thread::get_id(); });

  auto sender1 = start | ex::write_env(ex::prop {ncaf::get_scheduler_index, 0});
  auto sender2 = start | ex::write_env(ex::prop {ncaf::get_scheduler_index, 1});
  auto [thread_id1] = ex::sync_wait(sender1).value();
  auto [thread_id2] = ex::sync_wait(sender2).value();
  ASSERT_NE(thread_id1, thread_id2);

  auto coroutine = [&]() -> exec::task<void> {
    // create two actors, specify the scheduler index in ActorConfig.
    auto actor1 = co_await ncaf::Spawn<TestActor2>(ncaf::ActorConfig {.scheduler_index = 0});
    auto actor2 = co_await ncaf::Spawn<TestActor2>(ncaf::ActorConfig {.scheduler_index = 1});

    uint64_t thread_id1 = co_await actor1.Send<&TestActor2::GetThreadId>();
    uint64_t thread_id2 = co_await actor2.Send<&TestActor2::GetThreadId>();
    // the two actors should run on different thread pool
    EXPECT_NE(thread_id1, thread_id2);
  };
  ncaf::Init(scheduler_union.GetScheduler());
  ex::sync_wait(coroutine());
  ncaf::Shutdown();
}

TEST(SchedulerTest, TestResourceHolder) {
  auto shared_pool1 = std::make_unique<ncaf::WorkSharingThreadPool>(10);
  auto shared_pool2 = std::make_unique<ncaf::WorkSharingThreadPool>(10);
  auto union_pool = std::make_unique<ncaf::SchedulerUnion<ncaf::WorkSharingThreadPool::Scheduler>>(
      std::vector<ncaf::WorkSharingThreadPool::Scheduler> {shared_pool1->GetScheduler(), shared_pool2->GetScheduler()});
  ncaf::Init(union_pool->GetScheduler());
  ncaf::HoldResource(std::move(shared_pool1));
  ncaf::HoldResource(std::move(shared_pool2));
  ncaf::HoldResource(std::move(union_pool));
  auto coroutine = [&]() -> exec::task<void> {
    // create two actors, specify the scheduler index in ActorConfig.
    auto actor1 = co_await ncaf::Spawn<TestActor2>(ncaf::ActorConfig {.scheduler_index = 0});
    auto actor2 = co_await ncaf::Spawn<TestActor2>(ncaf::ActorConfig {.scheduler_index = 1});

    uint64_t thread_id1 = co_await actor1.Send<&TestActor2::GetThreadId>();
    uint64_t thread_id2 = co_await actor2.Send<&TestActor2::GetThreadId>();
    // the two actors should run on different thread pool
    EXPECT_NE(thread_id1, thread_id2);
  };
  ex::sync_wait(coroutine());
  ncaf::Shutdown();
}