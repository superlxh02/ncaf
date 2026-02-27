#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>

#include <exec/async_scope.hpp>
#include <exec/task.hpp>
#include <rfl/Tuple.hpp>
#include <rfl/apply.hpp>
#include <stdexec/execution.hpp>

#include "ncaf/internal/actor_config.h"
#include "ncaf/internal/reflect.h"
#include "ncaf/internal/util.h"

namespace ncaf::internal {

// actor消息基类
struct ActorMessage {
  virtual ~ActorMessage() = default;
  // 所有消息都通过Execute来执行
  virtual void Execute() = 0;
};

// 类型擦除的actor基类
class TypeErasedActor {
 public:
  explicit TypeErasedActor(ActorConfig actor_config) : actor_config_(std::move(actor_config)) {}
  virtual ~TypeErasedActor() = default;
  // 向actor邮箱推送消息
  virtual void PushMessage(ActorMessage* task) = 0;
  // 获取用户类实例的地址，方便反射调用
  virtual void* GetUserClassAddress() = 0;
  // 异步销毁actor，等待所有消息执行完毕
  virtual exec::task<void> AsyncDestroy() = 0;

  // 调用actor的方法，kMethod是方法的指针，args是方法的参数
  template <auto kMethod, class... Args>
  ex::sender auto CallActorMethod(Args... args);

  // 调用actor的方法，kMethod是方法的指针，args_tuple是方法的参数元组
  template <auto kMethod, class... Args>
  ex::sender auto CallActorMethodUseTuple(std::tuple<Args...> args_tuple);

  // 获取actor的配置
  const ActorConfig& GetActorConfig() const { return actor_config_; }

  // 从actor邮箱拉取消息并执行
  virtual void PullMailboxAndRun() = 0;

 protected:
  ActorConfig actor_config_;  // actor的配置
};

// 类型擦除的actor调度器基类
class TypeErasedActorScheduler {
 public:
  virtual ~TypeErasedActorScheduler() = default;
  // 调度actor执行，actor是被调度的actor实例，async_scope是调度器的async_scope，用于管理actor的生命周期
  virtual void Schedule(TypeErasedActor* actor, exec::async_scope& async_scope) = 0;
  // cloning接口，用于复制调度器实例，方便在不同actor之间共享调度器
  virtual std::unique_ptr<TypeErasedActorScheduler> Clone() const = 0;
  // 获取底层调度器的指针，用于类型转换
  virtual const void* GetUnderlyingSchedulerPtr() const = 0;
};

// 适配std::execution scheduler的actor调度器
// 继承自TypeErasedActorScheduler
template <ex::scheduler Scheduler>
class AnyStdExecScheduler : public TypeErasedActorScheduler {
 public:
  explicit AnyStdExecScheduler(Scheduler scheduler) : scheduler_(std::move(scheduler)) {}
  // 调度actor执行
  void Schedule(TypeErasedActor* actor, exec::async_scope& async_scope) override {
    // 从actor获取配置
    const auto& actor_config = actor->GetActorConfig();
    // 构造一个sender，用于调度actor执行
    // 先设置上下文，然后调度actor拉取消息并执行
    // 使用async_scope.spawn来调度actor执行
    auto sender = ex::schedule(scheduler_) | ex::write_env(ex::prop {ncaf::get_priority, actor_config.priority}) |
                  ex::write_env(ex::prop {ncaf::get_scheduler_index, actor_config.scheduler_index}) |
                  ex::then([actor] { actor->PullMailboxAndRun(); });
    async_scope.spawn(std::move(sender));
  }

  // clone接口，复制调度器实例
  std::unique_ptr<TypeErasedActorScheduler> Clone() const override {
    return std::make_unique<AnyStdExecScheduler<Scheduler>>(scheduler_);
  }

  // 获取底层调度器的指针，用于类型转换
  const void* GetUnderlyingSchedulerPtr() const override { return &scheduler_; }

 private:
  Scheduler scheduler_;  // 底层的std::execution scheduler实例
};

// actor消息提交scheduler
// 继承于ex::scheduler_t
struct StdExecSchedulerForActorMessageSubmission : public ex::scheduler_t {
  explicit StdExecSchedulerForActorMessageSubmission(TypeErasedActor* actor) : actor(actor) {}
  TypeErasedActor* actor;

  // 自定义初始的OperationState，将自己push到actor的邮箱中
  // 功能是封装start和excute接口
  template <class Receiver>
  struct ActorMessageSubmissionOperation : ActorMessage {
    TypeErasedActor* actor;
    Receiver receiver;
    ActorMessageSubmissionOperation(TypeErasedActor* actor, Receiver receiver)
        : actor(actor), receiver(std::move(receiver)) {}
    // 重载了Execute方法,协作停止
    void Execute() override {
      // 获取stop_token，如果stop_token不可停止，直接set_value，否则根据stop_token的状态决定是set_stopped还是set_value
      auto stoken = stdexec::get_stop_token(stdexec::get_env(receiver));
      // 如果stop_token不可停止，直接set_value
      if constexpr (ex::unstoppable_token<decltype(stoken)>) {
        receiver.set_value();
      } else {
        // 如果stop_token可停止，set_stopped否则set_value
        if (stoken.stop_requested()) {
          receiver.set_stopped();
        } else {
          receiver.set_value();
        }
      }
    }
    // 将消息提交到actor的邮箱
    void start() noexcept { actor->PushMessage(this); }
  };

  // 封装actor消息的sender，继承于ex::sender_t
  struct ActorMessageSubmissionSender : ex::sender_t {
    TypeErasedActor* actor;
    using completion_signatures = ex::completion_signatures<ex::set_value_t(), ex::set_stopped_t()>;
    // env结构体：用于描述一个actor的上下文环境
    struct Env {
      TypeErasedActor* actor;
      template <class CPO>
      auto query(ex::get_completion_scheduler_t<CPO>) const noexcept -> StdExecSchedulerForActorMessageSubmission {
        return StdExecSchedulerForActorMessageSubmission(actor);
      }
    };
    // 获取env
    auto get_env() const noexcept -> Env { return Env {.actor = actor}; }
    // connect方法，连接receiver和sender，返回一个actor消息提交操作类
    template <class Receiver>
    ActorMessageSubmissionOperation<Receiver> connect(Receiver receiver) noexcept {
      return {actor, std::move(receiver)};
    }
  };

  // 重载==操作符，比较两个scheduler是否相同，比较的标准是actor指针是否相同
  friend bool operator==(const StdExecSchedulerForActorMessageSubmission& lhs,
                         const StdExecSchedulerForActorMessageSubmission& rhs) noexcept {
    return lhs.actor == rhs.actor;
  }
  // 重载!=操作符，比较两个scheduler是否不同，比较的标准是actor指针是否不同
  ActorMessageSubmissionSender schedule() const noexcept { return {.actor = actor}; }
  // 获取前进保证，返回concurrent，表示可以并发执行
  auto query(ex::get_forward_progress_guarantee_t) const noexcept -> ex::forward_progress_guarantee {
    return ex::forward_progress_guarantee::concurrent;
  }
};

// actor类
template <class UserClass, auto kCreateFn = nullptr>
class Actor : public TypeErasedActor {
 public:
  template <typename... Args>
  explicit Actor(std::unique_ptr<TypeErasedActorScheduler> scheduler, ActorConfig actor_config, Args... args)
      : TypeErasedActor(std::move(actor_config)), scheduler_(std::move(scheduler)) {
    // 判断是否有自定义构建函数
    if constexpr (kCreateFn != nullptr) {
      user_class_instance_ = std::make_unique<UserClass>(kCreateFn(std::move(args)...));
    } else {
      user_class_instance_ = std::make_unique<UserClass>(std::move(args)...);
    }
  }

  // 使用参数元组创建actor实例，方便反射调用
  template <typename... Args>
  static std::unique_ptr<TypeErasedActor> CreateUseArgTuple(std::unique_ptr<TypeErasedActorScheduler> scheduler,
                                                            ActorConfig actor_config, std::tuple<Args...> arg_tuple) {
    return std::apply(
        [scheduler = std::move(scheduler), actor_config = std::move(actor_config)](auto&&... args) mutable {
          return std::make_unique<Actor<UserClass, kCreateFn>>(std::move(scheduler), std::move(actor_config),
                                                               std::move(args)...);
        },
        std::move(arg_tuple));
  }

  ~Actor() override = default;

  // 异步销毁actor，等待所有消息执行完毕
  /// Async destroy the actor, if there are still messages in the mailbox, they might not be processed.
  exec::task<void> AsyncDestroy() override {
    bool expected = false;
    // 使用CAS更新pending_to_be_destroyed_为true
    bool changed = pending_to_be_destroyed_.compare_exchange_strong(expected, true, std::memory_order_release,
                                                                    std::memory_order_acquire);
    if (!changed) {
      co_return;
    }
    // 待处理消息数量加1，防止新的消息被调度执行
    pending_message_count_.fetch_add(1, std::memory_order_release);
    // 把自己放到调度器上，触发消息执行，最终在PullMailboxAndRun中被销毁
    TryActivate();
    // 等待async_scope中的所有任务完成，确保所有消息都被执行完毕
    co_await async_scope_.on_empty();
  }

  // 向actor邮箱推送消息
  void PushMessage(ActorMessage* task) override {
    // 将消息推送到邮箱队列
    mailbox_.Push(task);
    pending_message_count_.fetch_add(1, std::memory_order_release);
    TryActivate();
  }

  void* GetUserClassAddress() override { return user_class_instance_.get(); }

 private:
  std::unique_ptr<TypeErasedActorScheduler> scheduler_;          // 调度器实例
  util::LinearizableUnboundedMpscQueue<ActorMessage*> mailbox_;  // 邮箱队列
  std::atomic_size_t pending_message_count_ = 0;                 // 待执行的消息数量
  std::unique_ptr<UserClass> user_class_instance_;               // 用户类实例
  exec::async_scope async_scope_;                     // 管理actor的生命周期，确保在销毁actor时等待所有消息执行完毕
  std::atomic_bool activated_ = false;                // 是否正在执行消息，防止重复调度
  std::atomic_bool pending_to_be_destroyed_ = false;  // 是否已经提交销毁请求，防止重复提交销毁请求

  // 把自己放到调度器上，触发消息执行
  void TryActivate() {
    bool expect = false;
    bool changed = activated_.compare_exchange_strong(expect, /*desired=*/true, /*success=*/std::memory_order_acq_rel,
                                                      /*failure=*/std::memory_order_acquire);
    if (!changed) {
      return;
    }
    scheduler_->Schedule(this, async_scope_);
  }

  // 从actor邮箱拉取消息并执行
  void PullMailboxAndRun() override {
    // 如果用户实例已经被销毁，丢弃所有消息并返回
    if (user_class_instance_ == nullptr) [[unlikely]] {
      size_t remaining = pending_message_count_.load(std::memory_order_acquire);
      logging::Warn("{} is already destroyed, but triggered again, it has {} messages remaining", Description(),
                    remaining);
      return;
    }

    // 如果已经提交销毁请求，丢弃所有消息并销毁actor实例
    if (pending_to_be_destroyed_.load(std::memory_order_acquire)) [[unlikely]] {
      user_class_instance_.reset();
      activated_.store(false, std::memory_order_release);
      size_t remaining = pending_message_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
      if (remaining > 0) {
        logging::Warn("{} is destroyed but still has {} messages remaining", Description(), remaining);
      }
      return;
    }

    // 轮训邮箱队列，执行消息，直到邮箱为空或者执行的消息数量达到actor_config_.max_message_executed_per_activation
    size_t message_executed = 0;
    while (auto optional_msg = mailbox_.TryPop()) {
      // 执行消息
      optional_msg.value()->Execute();
      message_executed++;
      if (message_executed >= actor_config_.max_message_executed_per_activation) [[unlikely]] {
        break;
      }
    }
    // 待执行的消息数量减去已经执行的消息数量
    pending_message_count_.fetch_sub(message_executed, std::memory_order_release);

    activated_.store(false, std::memory_order_seq_cst);

    if (pending_message_count_.load(std::memory_order_acquire) > 0) {
      TryActivate();
    }
  }

  // 获取actor的描述信息，包含地址、用户类类型和actor名字，方便日志输出
  std::string Description() {
    return fmt_lib::format("Actor {}(type:{},name:{})", (void*)this, typeid(UserClass).name(),
                           actor_config_.actor_name.value_or("null"));
  }
};  // class Actor

// 使用参数调用actor方法
template <auto kMethod, class... Args>
ex::sender auto TypeErasedActor::CallActorMethod(Args... args) {
  return CallActorMethodUseTuple<kMethod>(std::make_tuple(std::move(args)...));
}

// 使用参数元组调用actor方法
template <auto kMethod, class... Args>
ex::sender auto TypeErasedActor::CallActorMethodUseTuple(std::tuple<Args...> args_tuple) {
  using Sig = reflect::Signature<decltype(kMethod)>;
  using ReturnType = Sig::ReturnType;
  using UserClass = Sig::ClassType;
  // 判断返回类型是否是sender
  constexpr bool kIsNested = ex::sender<ReturnType>;
  // 将构造提交自己的sender
  auto start = ex::schedule(StdExecSchedulerForActorMessageSubmission(this));
  // 获取用户类实例的地址
  auto* user_class_instance = static_cast<UserClass*>(GetUserClassAddress());

  // 如果用户类型是sender
  if constexpr (kIsNested) {
    // 使用let_value,用户参数是一个tuple,需要使用std::apply展开参数调用用户方法,用户方法返回一个sender,直接返回这个sender
    return std::move(start) | ex::let_value([user_class_instance, args_tuple = std::move(args_tuple)]() mutable {
             return std::apply(
                 [user_class_instance](auto&&... args) { return (user_class_instance->*kMethod)(std::move(args)...); },
                 std::move(args_tuple));
           });
  } else {
    // 如果用户类型不是sender,使用then,用户参数是一个tuple,需要使用std::apply展开参数调用用户方法
    return std::move(start) | ex::then([user_class_instance, args_tuple = std::move(args_tuple)]() mutable {
             return std::apply(
                 [user_class_instance](auto&&... args) { return (user_class_instance->*kMethod)(std::move(args)...); },
                 std::move(args_tuple));
           });
  }
}
}  // namespace ncaf::internal
