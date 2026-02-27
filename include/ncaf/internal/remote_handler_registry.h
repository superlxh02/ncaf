#pragma once

#include <functional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "ncaf/internal/actor.h"
#include "ncaf/internal/actor_ref_serialization/actor_ref_serialization.h"  // IWYU pragma: keep
#include "ncaf/internal/network.h"
#include "ncaf/internal/reflect.h"
#include "ncaf/internal/serialization.h"

namespace ncaf::internal {

// 远程actor请求处理器注册表，负责注册和获取远程调用的处理器函数
class RemoteActorRequestHandlerRegistry {
 public:
  static RemoteActorRequestHandlerRegistry& GetInstance() {
    static RemoteActorRequestHandlerRegistry instance;
    return instance;
  }

  // 远程调用处理器上下文
  struct RemoteActorMethodCallHandlerContext {
    TypeErasedActor* actor;                                       // 被调用的actor实例
    serde::BufferReader<network::ByteBufferType> request_buffer;  // 调用请求的buffer，包含调用的方法名和参数等信息
    ActorRefDeserializationInfo info;                             // actor执行的上下文
  };

  // 远程创建actor处理器上下文
  struct RemoteActorCreationHandlerContext {
    serde::BufferReader<network::ByteBufferType> request_buffer;  // 请求buffer
    std::unique_ptr<TypeErasedActorScheduler> scheduler;          // 调度器
    ActorRefDeserializationInfo info;                             // actor创建的上下文
  };
  // 创建actor后的结果
  struct CreateActorResult {
    std::unique_ptr<TypeErasedActor> actor;
    std::optional<std::string> actor_name;
  };

  using RemoteActorMethodCallHandler =
      std::function<exec::task<network::ByteBufferType>(RemoteActorMethodCallHandlerContext context)>;
  using RemoteActorCreationHandler = std::function<CreateActorResult(RemoteActorCreationHandlerContext context)>;

  void RegisterRemoteActorMethodCallHandler(const std::string& key, RemoteActorMethodCallHandler func) {
    NCAF_THROW_CHECK(!remote_actor_method_call_handler_.contains(key))
        << "Duplicate remote actor method call handler: " << key
        << ", maybe you have duplicated functions in ncaf_remote.";
    remote_actor_method_call_handler_[key] = std::move(func);
  }

  RemoteActorMethodCallHandler GetRemoteActorMethodCallHandler(const std::string& key) const {
    NCAF_THROW_CHECK(remote_actor_method_call_handler_.contains(key))
        << "Remote actor method call handler not found: " << key
        << ", maybe you forgot to register it with ncaf_remote.";
    return remote_actor_method_call_handler_.at(key);
  }
  void RegisterRemoteActorCreationHandler(const std::string& key, RemoteActorCreationHandler func) {
    NCAF_THROW_CHECK(!remote_actor_creation_handler_.contains(key))
        << "Duplicate remote actor creation handler: " << key
        << ", maybe you have duplicated functions in ncaf_remote.";
    remote_actor_creation_handler_[key] = std::move(func);
  }
  RemoteActorCreationHandler GetRemoteActorCreationHandler(const std::string& key) const {
    NCAF_THROW_CHECK(remote_actor_creation_handler_.contains(key))
        << "Remote actor creation handler not found: " << key << ", maybe you forgot to register it with ncaf_remote.";
    return remote_actor_creation_handler_.at(key);
  }

 private:
  std::unordered_map<std::string, RemoteActorCreationHandler>
      remote_actor_creation_handler_;  // 远程创建actor的处理器映射表，key是方法名
  std::unordered_map<std::string, RemoteActorMethodCallHandler>
      remote_actor_method_call_handler_;  // 远程调用的处理器映射表，key是方法名
};

// 远程调用处理器注册器，用于向RemoteActorRequestHandlerRegistry注册远程调用处理器，使用模板参数指定创建函数和方法列表
template <auto kActorCreateFn, auto... kActorMethods>
class RemoteFuncHandlerRegistrar {
 public:
  explicit RemoteFuncHandlerRegistrar() {
    static_assert(!std::is_member_function_pointer_v<decltype(kActorCreateFn)>,
                  "kActorCreateFn must be a non-member function pointer");
    using CreateFnSig = reflect::Signature<decltype(kActorCreateFn)>;
    static_assert(!std::is_void_v<std::decay_t<typename CreateFnSig::ReturnType>>,
                  "kActorCreateFn must return a non-void value");
    static_assert(!std::is_fundamental_v<typename CreateFnSig::ReturnType>,
                  "kActorCreateFn must return a non-fundamental value");
    auto check_fn_class = []<class C, auto kMemberFn>() {
      using MemberFnSig = reflect::Signature<decltype(kMemberFn)>;
      static_assert(std::is_same_v<C, typename MemberFnSig::ClassType>,
                    "Actor methods' class does not match the create function's class");
    };

    // 折叠表达式，展开参数包，检查每一个方法的所属类是否和创建函数的返回类型一致
    (check_fn_class.template operator()<typename CreateFnSig::ReturnType, kActorMethods>(), ...);

    // 注册处理器的lambda函数
    auto register_handler = [this]<auto kFuncPtr>() {
      // 获取到调用函数名
      std::string func_name = reflect::GetUniqueNameForFunction<kFuncPtr>();
      // 根据是否是成员函数指针来注册不同的处理器
      if constexpr (std::is_member_function_pointer_v<decltype(kFuncPtr)>) {
        RemoteActorRequestHandlerRegistry::GetInstance().RegisterRemoteActorMethodCallHandler(
            func_name, [this](RemoteActorRequestHandlerRegistry::RemoteActorMethodCallHandlerContext context) {
              return DeserializeAndInvokeActorMethod<kFuncPtr>(std::move(context));
            });
      } else {
        RemoteActorRequestHandlerRegistry::GetInstance().RegisterRemoteActorCreationHandler(
            func_name, [this](RemoteActorRequestHandlerRegistry::RemoteActorCreationHandlerContext context) {
              return DeserializeAndCreateActor<kFuncPtr>(std::move(context));
            });
      }
    };
    // 注册创建函数和方法调用函数的处理器
    register_handler.template operator()<kActorCreateFn>();
    // 折叠表达式，展开参数包，注册每一个方法的处理器
    (register_handler.template operator()<kActorMethods>(), ...);
  }

 private:
  // 反序并且创建actor
  template <auto kCreateFn>
  RemoteActorRequestHandlerRegistry::CreateActorResult DeserializeAndCreateActor(
      RemoteActorRequestHandlerRegistry::RemoteActorCreationHandlerContext context) {
    using ActorClass = reflect::Signature<decltype(kCreateFn)>::ReturnType;
    // 反序列化得到创建actor需要的参数，包含actor配置和创建函数参数
    serde::ActorCreationArgs creation_args = serde::DeserializeFnArgs<kCreateFn>(
        context.request_buffer.Current(), context.request_buffer.RemainingSize(), context.info);

    // 构造actor实例
    std::unique_ptr<TypeErasedActor> actor = Actor<ActorClass, kCreateFn>::CreateUseArgTuple(
        std::move(context.scheduler), std::move(creation_args.actor_config), std::move(creation_args.args_tuple));
    // 获取actor_name
    auto actor_name = actor->GetActorConfig().actor_name;
    // 返回创建结果，包含actor实例和actor_name（如果有的话）
    return {
        .actor = std::move(actor),
        .actor_name = std::move(actor_name),
    };
  }

  // 返回一个协程，包含着该活动方法调用的序列化结果。
  template <auto kMethod>
  exec::task<network::ByteBufferType> DeserializeAndInvokeActorMethod(
      RemoteActorRequestHandlerRegistry::RemoteActorMethodCallHandlerContext context) {
    NCAF_THROW_CHECK(context.actor != nullptr);
    using Sig = reflect::Signature<decltype(kMethod)>;
    using UnwrappedType = decltype(reflect::UnwrapReturnSenderIfNested<kMethod>())::type;

    // 反序列化函数调用需要的参数
    serde::ActorMethodCallArgs<typename Sig::DecayedArgsTupleType> call_args = serde::DeserializeFnArgs<kMethod>(
        context.request_buffer.Current(), context.request_buffer.RemainingSize(), context.info);

    std::vector<char> serialized {};
    if constexpr (std::is_void_v<UnwrappedType>) {
      // 如果函数没有返回值，直接调用，不需要序列化返回值
      co_await context.actor->template CallActorMethodUseTuple<kMethod>(std::move(call_args.args_tuple));
    } else {
      // 如果函数有返回值，调用后需要序列化返回值
      auto return_value =
          co_await context.actor->template CallActorMethodUseTuple<kMethod>(std::move(call_args.args_tuple));
      serialized = serde::Serialize(serde::ActorMethodReturnValue<UnwrappedType> {std::move(return_value)});
    }
    // 构造协议数据包，预留长度，写入回应类型和返回值（如果有的话），返回buffer
    serde::BufferWriter writer(network::ByteBufferType {sizeof(serde::NetworkRequestType) + serialized.size()});
    writer.WritePrimitive(serde::NetworkReplyType::kActorMethodCallReturn);
    if constexpr (!std::is_void_v<UnwrappedType>) {
      writer.CopyFrom(serialized.data(), serialized.size());
    }
    co_return std::move(writer).MoveBufferOut();
  };
};

#define NCAF_CONCATENATE_DIRECT(s1, s2, s3, s4) s1##s2##s3##s4
#define NCAF_CONCATENATE(s1, s2, s3, s4) NCAF_CONCATENATE_DIRECT(s1, s2, s3, s4)
#define NCAF_ANONYMOUS_VARIABLE(str) NCAF_CONCATENATE(str, __LINE__, _, __COUNTER__)
}  // namespace ncaf::internal

namespace ncaf {
#define ncaf_remote(...)                          \
  /* NOLINTNEXTLINE(misc-use-internal-linkage) */ \
  inline ::ncaf::internal::RemoteFuncHandlerRegistrar<__VA_ARGS__> NCAF_ANONYMOUS_VARIABLE(exa_remote_func_registrar_);
};  // namespace ncaf
