#include <thread>

#include <gtest/gtest.h>

#include "ncaf/api.h"

TEST(UtilTest, SemaphoreBasicUsageCase) {
  ncaf::util::Semaphore semaphore(0);
  ncaf::ex::sync_wait(semaphore.OnDrained());
  ASSERT_EQ(semaphore.Release(10), 10);
  std::jthread t([&semaphore]() {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    ASSERT_EQ(semaphore.Acquire(11), -1);
  });
  ncaf::ex::sync_wait(semaphore.OnDrained());
}