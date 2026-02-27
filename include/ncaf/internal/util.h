#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <unordered_set>

#include <spdlog/spdlog.h>
#include <stdexec/execution.hpp>

#include "ncaf/3rd_lib/daking/MPSC_queue.h"
#include "ncaf/3rd_lib/moody_camel_queue/blockingconcurrentqueue.h"
#include "ncaf/internal/alias.h"
#include "ncaf/internal/logging.h"

namespace ncaf::util {
// 封装一个信号量，提供一个sender，当信号量被耗尽时完成
class Semaphore {
 public:
  explicit Semaphore(int64_t initial_count) : count_(initial_count) {}

  // acq语义：当信号量被耗尽时，完成所有等待的sender，并返回剩余的信号量数量(可能为负数)
  int64_t Acquire(int64_t n) {
    auto prev = count_.fetch_sub(n, std::memory_order_release);
    if (prev <= n) {
      std::unique_lock lock(waiters_mu_);
      auto waiters = std::move(waiters_);
      lock.unlock();
      for (auto* waiter : waiters) {
        waiter->Complete();
      }
    }
    return prev - n;
  }

  // rel语义：增加信号量数量，并不完成等待的sender
  int64_t Release(int64_t n) {
    auto prev = count_.fetch_add(n, std::memory_order_release);
    return prev + n;
  }

  // 获取当前信号量数量
  int64_t CurrentValue() const { return count_.load(std::memory_order_acquire); }

  struct OnDrainedSender;

  // 返回一个sender，当信号量被耗尽(value <= 0)时完成
  OnDrainedSender OnDrained() { return OnDrainedSender {.semaphore = this}; }

  /*

  一。以下类的实现是为了实现OnDrained的返回值,和上层用户无关,用户只需要调用OnDrained()获取一个sender,当信号量被耗尽时这个sender完成即可
  即当信号量被耗尽时，完成OnDrainedSender连接的receiver
  用于等待信号量被耗尽的场景，例如等待所有actor销毁完成，或者等待所有消息处理完成等

  二。实现一个自定义sender的通用步骤：
  1.写自定义OprateionState类，实现start方法
  2.写自定义sender类，继承于ex::sender_t，定义completion_signatures，写connect方法
  */

  // 类型擦除OperateState基类
  struct TypeErasedOnDrainedOperation {
    virtual ~TypeErasedOnDrainedOperation() = default;
    virtual void Complete() = 0;
  };

  // 等待信号量被耗尽的OperateState，持有一个receiver，当信号量被耗尽时调用receiver.set_value()
  template <ex::receiver R>
  struct OnDrainedOperation : public TypeErasedOnDrainedOperation {
    OnDrainedOperation(Semaphore* semaphore, R receiver) : semaphore(semaphore), receiver(std::move(receiver)) {}
    Semaphore* semaphore;
    R receiver;

    // 启动等待，如果信号量已经被耗尽，直接完成，否则将自己加入等待队列
    void start() noexcept {
      std::scoped_lock lock(semaphore->waiters_mu_);
      if (semaphore->count_.load(std::memory_order_acquire) <= 0) {
        Complete();
      } else {
        semaphore->waiters_.push_back(this);
      }
    }

    void Complete() override { receiver.set_value(); }
  };

  // 等待信号量被耗尽的sender，connect时创建一个OnDrainedOperation，并启动等待
  struct OnDrainedSender : ex::sender_t {
    using completion_signatures = ex::completion_signatures<ex::set_value_t()>;
    Semaphore* semaphore;

    template <ex::receiver R>
    OnDrainedOperation<R> connect(R receiver) {
      return {semaphore, std::move(receiver)};
    }
  };

 private:
  mutable std::mutex waiters_mu_;                       // 互斥锁
  std::vector<TypeErasedOnDrainedOperation*> waiters_;  // 等待信号量被耗尽的操作队列
  std::atomic_int64_t count_;                           // 引用计数
};

};  // namespace ncaf::util

namespace ncaf::internal::util {

// 基于daking的无界多生产者单消费者队列，提供Push和TryPop接口
template <class T>
struct LinearizableUnboundedMpscQueue {
 public:
  void Push(T value) { queue_.enqueue(std::move(value)); }

  std::optional<T> TryPop() {
    T value;
    if (queue_.try_dequeue(value)) {
      return value;
    }
    return std::nullopt;
  }

 private:
  ncaf::embedded_3rd::daking::MPSC_queue<T> queue_;
};

// 基于std::priority_queue和条件变量实现的无界阻塞优先级队列，提供Push和Pop接口
template <class T>
class UnboundedBlockingPriorityQueue {
 public:
  void Push(T value, uint32_t priority) {
    std::lock_guard lock(mutex_);
    queue_.push({std::move(value), priority});
    cv_.notify_one();
  }

  std::optional<T> Pop(uint64_t timeout_ms) {
    std::unique_lock lock(mutex_);
    bool ok = cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return !queue_.empty(); });
    if (!ok) {
      return std::nullopt;
    }
    auto value = std::move(const_cast<Element&>(queue_.top()).value);
    queue_.pop();
    return value;
  }

 private:
  struct Element {
    T value;
    uint32_t priority;
    friend bool operator<(const Element& lhs, const Element& rhs) { return lhs.priority > rhs.priority; }
  };
  std::priority_queue<Element> queue_;
  std::mutex mutex_;
  std::condition_variable cv_;
};

// 基于moodycamel的BlockingConcurrentQueue实现的无界阻塞队列，提供Push和Pop接口
template <class T>
class UnboundedBlockingQueue {
 public:
  void Push(T value) { queue_.enqueue(std::move(value)); }
  std::optional<T> Pop(uint64_t timeout_ms) {
    T value;
    bool ok = queue_.wait_dequeue_timed(value, std::chrono::milliseconds(timeout_ms));
    if (!ok) {
      return std::nullopt;
    }
    return value;
  }

 private:
  ncaf::embedded_3rd::moodycamel::BlockingConcurrentQueue<T> queue_;
};

// 基于std::unordered_map和std::mutex实现的线程安全map，提供Insert、At、Erase、Contains接口
template <class K, class V>
class LockGuardedMap {
 public:
  bool Insert(const K& key, V value) {
    std::lock_guard lock(mutex_);
    auto [iter, inserted] = map_.try_emplace(key, std::move(value));
    return inserted;
  }

  V& At(const K& key) {
    std::lock_guard lock(mutex_);
    auto iter = map_.find(key);
    NCAF_THROW_CHECK(iter != map_.end()) << "Key not found: " << key;
    return iter->second;
  }

  const V& At(const K& key) const {
    std::lock_guard lock(mutex_);
    auto iter = map_.find(key);
    NCAF_THROW_CHECK(iter != map_.end()) << "Key not found: " << key;
    return iter->second;
  }

  void Erase(const K& key) {
    std::lock_guard lock(mutex_);
    map_.erase(key);
  }

  bool Contains(const K& key) const {
    std::lock_guard lock(mutex_);
    return map_.contains(key);
  }

  void Clear() {
    std::lock_guard lock(mutex_);
    map_.clear();
  }

  std::unordered_map<K, V>& GetMap() { return map_; }
  std::mutex& GetMutex() const { return mutex_; }

 private:
  std::unordered_map<K, V> map_;
  mutable std::mutex mutex_;
};

// 基于std::unordered_set和std::mutex实现的线程安全set，提供Insert、Erase、Empty、Contains接口
template <class T>
class LockGuardedSet {
 public:
  bool Insert(T value) {
    std::lock_guard lock(mutex_);
    auto [iter, inserted] = set_.emplace(std::move(value));
    return inserted;
  }

  void Erase(const T& value) {
    std::lock_guard lock(mutex_);
    set_.erase(value);
  }

  bool Empty() {
    std::lock_guard lock(mutex_);
    return set_.empty();
  }

  bool Contains(T value) {
    std::lock_guard lock(mutex_);
    return set_.contains(value);
  }

  std::mutex& GetMutex() const { return mutex_; }

 private:
  std::unordered_set<T> set_;
  mutable std::mutex mutex_;
};

// 一个只能移动不能复制的any类型，提供MoveValueOut接口将值移动出去
class MoveOnlyAny {
 public:
  MoveOnlyAny(const MoveOnlyAny&) = delete;
  MoveOnlyAny& operator=(const MoveOnlyAny&) = delete;
  MoveOnlyAny(MoveOnlyAny&&) = default;
  MoveOnlyAny& operator=(MoveOnlyAny&&) = default;

  MoveOnlyAny() : value_holder_(nullptr) {}
  template <class T>
  explicit MoveOnlyAny(T value) : value_holder_(std::make_unique<AnyValueHolderImpl<T>>(std::move(value))) {}
  template <class T>
  MoveOnlyAny& operator=(T value) {
    value_holder_ = std::make_unique<AnyValueHolderImpl<T>>(std::move(value));
    return *this;
  }

  template <class T>
  T&& MoveValueOut() && {
    return std::move(static_cast<AnyValueHolderImpl<T>*>(value_holder_.get())->value);
  }

 private:
  struct AnyValueHolder {
    virtual ~AnyValueHolder() = default;
  };

  template <class T>
  struct AnyValueHolderImpl : public AnyValueHolder {
    T value;
    explicit AnyValueHolderImpl(T value) : value(std::move(value)) {}
  };
  std::unique_ptr<AnyValueHolder> value_holder_;
};

#if defined(_WIN32)
inline void SetThreadName(const std::string& name) {
  // TODO
}
#elif defined(__linux__)
#include <pthread.h>
inline void SetThreadName(const std::string& name) { pthread_setname_np(pthread_self(), name.c_str()); }
#else
inline void SetThreadName(const std::string&) {}
#endif

// 将一个sender包装成在inline_scheduler上执行的sender，适用于需要在当前线程继续执行的场景
inline auto WrapSenderWithInlineScheduler(auto task) {
  return std::move(task) | stdexec::write_env(ex::prop {stdexec::get_scheduler, stdexec::inline_scheduler {}});
}
}  // namespace ncaf::internal::util
