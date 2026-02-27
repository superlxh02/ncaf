#include <cassert>
#include <iostream>
#include <stdexcept>

#include <gmock/gmock-matchers.h>
#include <gtest/gtest.h>

#include "ncaf/api.h"
#include "ncaf/internal/actor_registry.h"
#include "rfl/internal/has_reflector.hpp"

namespace ex = stdexec;

using testing::HasSubstr;
using testing::Property;
using testing::Throws;

class Counter {
public:
  void Add(int x) { count_ += x; }
  int GetValue() const { return count_; }

  void Error() const {
    throw std::runtime_error("error" + std::to_string(count_));
  }

private:
  int count_ = 0;
};

class Proxy {
public:
  explicit Proxy(ncaf::ActorRef<Counter> actor_ref) : actor_ref_(actor_ref) {}

  exec::task<int> GetValue() {
    int res = co_await actor_ref_.template Send<&Counter::GetValue>();
    std::cout << "This line runs on the current actor(Proxy), because "
                 "coroutine has scheduler affinity\n";
    co_return res;
  }

  ex::sender auto GetValue2() {
    return actor_ref_.template Send<&Counter::GetValue>() |
           ex::then([](int value) {
             std::cout << "This line runs on the target actor(Counter), unless "
                          "you call continue_on explicitly.\n";
             return value;
           });
  }

private:
  ncaf::ActorRef<Counter> actor_ref_;
};

TEST(BasicApiTest, ActorRegistryCreationWithDefaultScheduler) {
  auto coroutine = []() -> exec::task<void> {
    auto counter = co_await ncaf::Spawn<Counter>();
    auto getvalue_sender = counter.Send<&Counter::GetValue>();
    auto getvalue_reply = co_await std::move(getvalue_sender);
    EXPECT_EQ(getvalue_reply, 0);
    co_return;
  };
  ncaf::Init(/*thread_pool_size=*/10);
  ex::sync_wait(coroutine());
  ncaf::Shutdown();
}

TEST(BasicApiTest, ShouldWorkWithAsyncSpawn) {
  auto coroutine = []() -> exec::task<void> {
    auto counter = co_await ncaf::Spawn<Counter>();
    exec::async_scope scope;
    scope.spawn(counter.SendLocal<&Counter::Add>(1));
    auto future = scope.spawn_future(counter.SendLocal<&Counter::GetValue>());
    auto res = co_await std::move(future);
    EXPECT_EQ(res, 1);
    // Wait for all spawned work to complete before destroying the scope
    co_await scope.on_empty();
    co_return;
  };
  ncaf::Init(/*thread_pool_size=*/10);
  ex::sync_wait(coroutine());
  ncaf::Shutdown();
}

TEST(BasicApiTest, ExceptionInActorMethodShouldBePropagatedToCaller) {
  auto coroutine = []() -> exec::task<void> {
    auto counter = co_await ncaf::Spawn<Counter>();
    co_await counter.Send<&Counter::Error>();
  };
  ncaf::Init(/*thread_pool_size=*/10);
  ASSERT_THAT([&coroutine] { ex::sync_wait(coroutine()); },
              Throws<std::exception>(
                  Property(&std::exception::what, HasSubstr("error0"))));
  ncaf::Shutdown();
}

TEST(BasicApiTest, NestActorRefCase) {
  auto coroutine = []() -> exec::task<void> {
    ncaf::ActorRef counter = co_await ncaf::Spawn<Counter>();
    exec::async_scope scope;
    for (int i = 0; i < 100; ++i) {
      scope.spawn(counter.Send<&Counter::Add>(1));
    }
    auto res = co_await counter.Send<&Counter::GetValue>();
    EXPECT_EQ(res, 100);

    ncaf::ActorRef proxy = co_await ncaf::Spawn<Proxy>(counter);
    auto res2 = co_await proxy.Send<&Proxy::GetValue>();
    auto res3 = co_await proxy.Send<&Proxy::GetValue2>();
    EXPECT_EQ(res2, 100);
    EXPECT_EQ(res3, 100);
    co_return;
  };
  ncaf::Init(/*thread_pool_size=*/10);
  ex::sync_wait(coroutine());
  ncaf::Shutdown();
}

TEST(BasicApiTest, CreateActorWithFullConfig) {
  auto coroutine = []() -> exec::task<void> {
    ncaf::ActorConfig config1{.max_message_executed_per_activation = 10,
                              .actor_name = "counter1"};
    auto counter = co_await ncaf::Spawn<Counter>(config1);

    ncaf::ActorConfig config2{.actor_name = "counter2"};
    co_await ncaf::Spawn<Counter>(config2);

    co_await ncaf::Spawn<Counter>(
        ncaf::ActorConfig{.scheduler_index = 0, .priority = 1});

    static_assert(rfl::internal::has_read_reflector<ncaf::ActorRef<Counter>>);
    static_assert(rfl::internal::has_write_reflector<ncaf::ActorRef<Counter>>);
    // test pass by lvalue
    ncaf::ActorConfig config = {.max_message_executed_per_activation = 10};
    co_await ncaf::Spawn<Proxy>(config, counter);
    co_await ncaf::DestroyActor(counter);
  };
  ncaf::Init(/*thread_pool_size=*/10);
  ex::sync_wait(coroutine());
  ncaf::Shutdown();
}

class TestActorWithNamedLookup {
public:
  exec::task<ncaf::ActorRef<Counter>> LookUpActor() {
    co_return (co_await ncaf::GetActorRefByName<Counter>("counter")).value();
  }
};

TEST(BasicApiTest, LookUpNamedActor) {
  auto coroutine = []() -> exec::task<void> {
    ncaf::ActorConfig config{.actor_name = "counter"};
    co_await ncaf::Spawn<Counter>(config);
    auto test_retriever_actor =
        co_await ncaf::Spawn<TestActorWithNamedLookup>();

    auto lookup_sender =
        test_retriever_actor.Send<&TestActorWithNamedLookup::LookUpActor>();
    auto lookup_reply = co_await std::move(lookup_sender);

    auto actor = lookup_reply;
    auto getvalue_sender = actor.Send<&Counter::GetValue>();
    auto getvalue_reply = co_await std::move(getvalue_sender);
    EXPECT_EQ(getvalue_reply, 0);
  };
  ncaf::Init(/*thread_pool_size=*/10);
  ex::sync_wait(coroutine());
  ncaf::Shutdown();
}
