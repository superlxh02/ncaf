#pragma once

#include <cstddef>
#include <tuple>
#include <type_traits>

#include <exec/task.hpp>
#include <rfl/Tuple.hpp>
#include <stdexec/execution.hpp>

namespace ncaf::internal::reflect {

template <typename T>
struct Signature;

template <class R, class... Args>
struct Signature<R (*)(Args...)> {
  using ReturnType = R;
  using ArgsTupleType = std::tuple<Args...>;
  using DecayedArgsTupleType = std::tuple<std::decay_t<Args>...>;
};

template <typename R, typename C, typename... Args>
struct Signature<R (C::*)(Args...)> {
  using ClassType = C;
  using ReturnType = R;
  using ArgsTupleType = std::tuple<Args...>;
  using DecayedArgsTupleType = std::tuple<std::decay_t<Args>...>;
};
template <typename R, typename C, typename... Args>
struct Signature<R (C::*)(Args...) const> : Signature<R (C::*)(Args...)> {};
template <typename R, typename C, typename... Args>
struct Signature<R (C::* const)(Args...)> : Signature<R (C::*)(Args...)> {};
template <typename R, typename C, typename... Args>
struct Signature<R (C::* const)(Args...) const> : Signature<R (C::*)(Args...)> {};

// 变量模板，用来判断一个类型是否是某个模板的特化，默认是false
template <class This, template <class...> class Other>
constexpr bool kIsSpecializationOf = false;

// 变量模板，偏特化情况，当This是Other的一个特化时，值为true
template <template <class...> class Template, class... Args>
constexpr bool kIsSpecializationOf<Template<Args...>, Template> = true;

// 概念约束，判断一个类型是否是某个模板的特化
template <class This, template <class...> class Other>
concept SpecializationOf = kIsSpecializationOf<std::decay_t<This>, Other>;

// 判断两个成员函数指针是否相同，注意即使它们的类型相同，也不一定是同一个函数，所以还要比较它们的值
template <auto kF1, auto kF2>
constexpr bool IsSameMemberFn() {
  if constexpr (!std::is_same_v<decltype(kF1), decltype(kF2)>) {
    return false;
  } else {
    return kF1 == kF2;
  }
}

template <class T>
struct ExTaskTraits;
// 假设exec::task是一个模板类，ExTaskTraits用来提取它的内层类型
template <class T>
struct ExTaskTraits<exec::task<T>> {
  using InnerType = T;
};

// 概念约束，约束一个range的范围
template <class R, class T>
concept RangeOf = std::ranges::range<R> && std::same_as<std::ranges::range_value_t<R>, T>;

// 在一个参数包中查找某个类型，返回它的索引，如果找不到就返回std::nullopt
template <typename T, typename... Ts>
consteval std::optional<size_t> GetIndexInParamPack() {
  if constexpr (sizeof...(Ts) != 0) {
    constexpr bool kMatches[] = {std::is_same_v<T, Ts>...};
    for (std::size_t i = 0; i < sizeof...(Ts); ++i) {
      if (kMatches[i]) return i;
    }
  }
  return std::nullopt;
}

// 在一个参数包中获取某个索引的类型，如果索引越界就编译错误
template <size_t kIndex, class... Ts>
using ParamPackElement = std::tuple_element_t<kIndex, std::tuple<Ts...>>;

// ---------------- Actor Methods Related ----------------
#ifndef NCAF_ACTOR_METHODS_KEYWORD
#define NCAF_ACTOR_METHODS_KEYWORD kActorMethods
#endif

template <class T>
constexpr std::tuple NCAF_ACTOR_METHODS_KEYWORD = {};

// 定义一个变量模板，辅助用来判断一个类是否定义了静态成员函数，默认继承自false_type
template <typename, typename = void>
struct HasStaticMemberActorMethods : std::false_type {};
// 偏特化，如果T有一个静态成员函数叫NCAF_ACTOR_METHODS_KEYWORD，那么这个特化就会被选中，值为true
// 原理是只有静态函数才可以通过类名访问，所以如果T::NCAF_ACTOR_METHODS_KEYWORD存在，那么它一定是一个静态成员函数
template <typename T>
struct HasStaticMemberActorMethods<T, std::void_t<decltype(T::NCAF_ACTOR_METHODS_KEYWORD)>> : std::true_type {};

// 主变量模板，判断一个类是否定义了静态成员函数
template <typename T>
inline constexpr bool kHasStaticMemberActorMethods = HasStaticMemberActorMethods<T>::value;

// 获取到一个actor类定义的所有actor方法组成的tuple，如果没有定义就返回一个空的tuple
template <class UserClass>
consteval auto GetActorMethodsTuple() {
  if constexpr (kHasStaticMemberActorMethods<UserClass>) {
    return UserClass::NCAF_ACTOR_METHODS_KEYWORD;
  } else {
    return NCAF_ACTOR_METHODS_KEYWORD<UserClass>;
  }
}
// 判断有没有定义actor方法
template <class T>
concept HasActorMethods = (std::tuple_size_v<decltype(GetActorMethodsTuple<T>())> > 0);

// 使用consteval强制编译期检查是否定义了actor方法
template <class UserClass>
consteval void CheckHasActorMethods() {
  static_assert(HasActorMethods<UserClass>,
                "Actor class must define constexpr static member `kActorMethods`, which is a std::tuple contains "
                "method pointers to your actor methods.");
}

// 递归寻找一个成员函数指针在一个actor方法tuple中的索引，如果找到了就返回索引，否则返回std::nullopt
template <auto kFunc, size_t kIndex = 0>
  requires std::is_member_function_pointer_v<decltype(kFunc)>
consteval std::optional<uint64_t> GetActorMethodIndex() {
  using ClassType = Signature<decltype(kFunc)>::ClassType;
  constexpr auto kActorMethodsTuple = GetActorMethodsTuple<ClassType>();
  if constexpr (std::tuple_size_v<decltype(kActorMethodsTuple)> == 0) {
    return std::nullopt;
  } else if constexpr (IsSameMemberFn<std::get<kIndex>(kActorMethodsTuple), kFunc>()) {
    return kIndex;
  } else if constexpr (kIndex + 1 < std::tuple_size_v<decltype(kActorMethodsTuple)>) {
    return GetActorMethodIndex<kFunc, kIndex + 1>();
  }
  return std::optional<uint64_t> {};
};

// 编译期推导协程co_await的返回类型
template <stdexec::sender Sender>
using CoAwaitType =
    decltype(std::declval<exec::task<void>::promise_type>().await_transform(std::declval<Sender>()).await_resume());

// 判断一个awaitable的co_await结果是否是某个类型
template <class Awaitable, class T>
concept AwaitableOf = std::is_same_v<CoAwaitType<Awaitable>, T>;

// 在编译期分析一个成员函数指针 kMethod 的返回类型，并“穿透”一层 sender 包装（如果存在），返回其最终产出的值类型。
template <auto kMethod>
constexpr auto UnwrapReturnSenderIfNested() {
  using ReturnType = Signature<decltype(kMethod)>::ReturnType;
  constexpr bool kIsNested = stdexec::sender<ReturnType>;
  if constexpr (kIsNested) {
    return std::type_identity<CoAwaitType<ReturnType>> {};
  } else {
    return std::type_identity<ReturnType> {};
  }
}

template <auto kFunc>
std::string GetUniqueNameForFunction() {
  return typeid(kFunc).name();
}
}  // namespace ncaf::internal::reflect

namespace ncaf::reflect {
using ::ncaf::internal::reflect::NCAF_ACTOR_METHODS_KEYWORD;
}  // namespace ncaf::reflect
