ncaf - Neo C++ Actor Framework
============================

## 项目简介

`ncaf` 是一个基于现代C++的跨平台分布式 Actor 框架。主要基于 C++20 协程和 `stdexec` 构建，支持在**单进程**和**多节点分布式集群**中创建、调度和通信 Actor。设计了基于无侵入式的API接口，可以像写普通 C++ 类一样编写业务逻辑，然后将其实例托管给 `ncaf` 运行时，通过 `ActorRef<T>` 在本地或远程安全地实现高并发、易扩展的服务。

## 项目特点

- **基于 stdexec 的统一异步模型**

  - 全面采用 `stdexec` 发送者 / 接收者模型与 `exec::task<T>`，在本地调用、远程调用、网络收发、线程池调度等场景下使用同一套异步抽象。
  - Actor 方法既可以返回普通值，也可以返回 `exec::task<T>`，框架会自动展开嵌套 sender，保证调用方总是以统一的 `co_await` 形式消费结果。
- **可插拔的多场景调度器架构**

  - 内置工作共享线程池、优先级线程池、work-stealing 线程池以及 `SchedulerUnion` 组合调度器，适配 CPU 密集、IO 密集、混合负载等不同场景。
  - 通过 `ActorConfig.scheduler_index`、`priority` 等配置，业务可以细粒度控制不同 Actor 的运行位置与优先级，而无需感知底层实现。
- **基于 ZeroMQ 的透明分布式组网**

  - 使用 ZeroMQ（`libzmq` + `cppzmq`）实现节点间的请求 / 响应通道与心跳机制，开发者只需配置 `NodeInfo` 集群列表即可接入分布式。
  - 远程 Actor 创建与方法调用与本地接口保持一致，调用者只需拿到 `ActorRef<T>` 并调用 `Send`，底层自动完成序列化、路由和重组。
- **基于类型擦除的通用接口封装**

  - `internal::TypeErasedActor` 与 `TypeErasedActorScheduler` 对用户自定义类与调度器做类型擦除，运行时仅操作统一的抽象接口。
  - 通过反射与模板元编程自动生成调用包装器，避免虚函数层层下钻，既降低模板爆炸，又保留跨 Actor 类型的统一调度与生命周期管理。
- **非侵入式 API 设计**

  - 业务 Actor 只需写成普通 C++ 类，无需继承框架基类或写宏，构造函数与成员方法完全由业务自行定义。
  - 通过自由函数 + `ncaf_remote(...)` 注册远程工厂和方法，既保持接口简洁，又支持分布式场景下的远程创建和远程调用。
- **业务层零锁并发优化**

  - Actor 内部通过单线程串行执行消息，业务逻辑无需显式加锁即可在高并发下保持数据一致性。
  - 框架使用无锁 / 低锁的 MPSC 队列、优先级队列等数据结构，实现多线程调度与跨节点通信，最大限度减少锁竞争对业务代码的影响。

## 项目环境

- **操作系统**

  - Linux / macOS/Windows
- **编译器**

  - 支持 C++20 及以上标准的现代编译器。
  - 推荐较新的 Clang / GCC（协程支持较好，测试中对 GCC 13 及以上做了行为说明）。
- **构建系统**

  - 使用 CMake 进行配置与构建。
- **主要第三方依赖（由 CMake 自动拉取与构建）**

  - `stdexec`：C++ 发送者 / 接收者执行模型实现。
  - `reflect-cpp` + Cap'n Proto：Actor 消息与 `ActorRef` 的序列化 / 反序列化。
  - `libzmq` + `cppzmq`：分布式模式下的网络通信与请求 / 响应。
  - `spdlog`：日志系统。
  - `googletest` / `googlemock`：仅用于测试。

通常情况下，直接使用项目自带的 CMake 脚本即可自动下载并构建依赖，无需手工安装：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
```

## 简单示例

下面是一个最小可运行的单节点示例，展示如何：

1. 定义一个普通的 C++ 类 `Counter` 作为 Actor。
2. 使用全局运行时 `ncaf::Init` / `ncaf::Shutdown`。
3. 通过 `ncaf::Spawn<T>` 创建 Actor，并用 `ActorRef<T>::Send` 发送消息。

```cpp
#include <iostream>

#include "ncaf/api.h"

// 业务类：作为 Actor 托管到 ncaf 运行时
class Counter {
 public:
  void Add(int x) { value_ += x; }
  int GetValue() const { return value_; }

 private:
  int value_ = 0;
};

// 主协程：演示创建 Actor 并发送消息
exec::task<void> MainCoroutine() {
  // 在当前进程内创建一个 Counter Actor
  ncaf::ActorRef<Counter> counter = co_await ncaf::Spawn<Counter>();

  // 通过 ActorRef 发送消息；Send 返回一个 sender，可被 co_await
  co_await counter.Send<&Counter::Add>(1);
  co_await counter.Send<&Counter::Add>(2);

  int value = co_await counter.Send<&Counter::GetValue>();
  std::cout << "Counter value = " << value << std::endl;  // 输出 3
}

int main() {
  // 使用默认工作线程池初始化全局运行时（例如 4 个工作线程）
  ncaf::Init(/*thread_pool_size=*/4);

  // 启动主协程
  stdexec::sync_wait(MainCoroutine());

  // 显式关闭运行时，等待所有 Actor 安全销毁
  ncaf::Shutdown();
}
```

## 项目 API 使用

### 1. 初始化与关闭运行时

- **单节点（使用默认线程池）**

```cpp
// 使用 N 个工作线程创建默认工作共享线程池
ncaf::Init(/*thread_pool_size=*/N);
// ...
ncaf::Shutdown();
```

- **单节点（自定义调度器）**

```cpp
ncaf::WorkSharingThreadPool pool(8);  // 或 PriorityThreadPool / WorkStealingThreadPool
ncaf::Init(pool.GetScheduler());
// 为保证生命周期安全，可将 pool 包到 shared_ptr 中并调用 HoldResource
ncaf::HoldResource(std::make_shared<ncaf::WorkSharingThreadPool>(std::move(pool)));
// ...
ncaf::Shutdown();
```

- **分布式模式**

```cpp
std::vector<ncaf::NodeInfo> cluster = {
    {.node_id = 0, .address = "tcp://127.0.0.1:5301"},
    {.node_id = 1, .address = "tcp://127.0.0.1:5302"},
};

uint32_t this_node_id = /* 当前节点 id */;

// 使用默认线程池
ncaf::Init(/*thread_pool_size=*/8, this_node_id, cluster);

// 或使用自定义调度器
// ncaf::Init(pool.GetScheduler(), this_node_id, cluster);
```

### 2. 定义 Actor 类

- Actor 本质上是一个普通的 C++ 类：
  - 构造函数可以是无参、带值类型参数、甚至 move-only 参数。
  - 成员方法可以是：
    - 普通同步方法，返回值或 `void`。
    - 返回 `exec::task<T>` 的协程方法。

```cpp
class MyActor {
 public:
  explicit MyActor(std::string name) : name_(std::move(name)) {}

  void Add(int x) { value_ += x; }
  int GetValue() const { return value_; }

  exec::task<int> GetValueAsync() {
    co_return value_;
  }

 private:
  std::string name_;
  int value_ = 0;
};
```

如果需要支持**远程创建与远程方法调用**，可以提供一个非成员的工厂函数并用 `ncaf_remote` 注册（分布式部分详见测试用例与源码）。

### 3. 创建 Actor：`ncaf::Spawn`

- **使用默认配置创建本地 Actor**

```cpp
auto actor = co_await ncaf::Spawn<MyActor>("demo");
```

- **使用 `ActorConfig` 进行更细粒度控制**

```cpp
ncaf::ActorConfig config{
    .max_message_executed_per_activation = 10,
    .actor_name = "my_actor",       // 可选：用于名字查找
    .scheduler_index = 0,           // 可选：用于 SchedulerUnion 指定调度器索引
    .priority = 1,                  // 可选：用于优先级线程池
};

auto actor = co_await ncaf::Spawn<MyActor>(config, "demo");
```

在分布式模式下，还可以通过设置 `config.node_id` 将 Actor 创建到远程节点（配合 `ncaf_remote` 注册的工厂函数）。

### 4. 发送消息：`ActorRef<T>::Send` / `SendLocal`

- **统一的 `Send` 接口（本地 / 远程均可）**

```cpp
// 发送同步方法
int value = co_await actor.Send<&MyActor::GetValue>();

// 发送返回协程的异步方法（内部会自动展开等待）
int value2 = co_await actor.Send<&MyActor::GetValueAsync>();

// 发送 void 返回值方法
co_await actor.Send<&MyActor::Add>(10);
```

- **仅限本地 Actor 的高性能 `SendLocal`**

```cpp
// 要求该 ActorRef 指向本地 Actor，否则会抛异常
auto sender = actor.SendLocal<&MyActor::Add>(1);
co_await std::move(sender);
```

### 5. 销毁 Actor 与名字查找

- **显式销毁 Actor**

```cpp
co_await ncaf::DestroyActor(actor);
```

- **按名字查找 Actor**

```cpp
// 创建时指定 actor_name 后，可通过名字查找
ncaf::ActorConfig config{.actor_name = "counter"};
auto counter = co_await ncaf::Spawn<MyActor>(config, "demo");

auto found = co_await ncaf::GetActorRefByName<MyActor>("counter");
if (found.has_value()) {
  int v = co_await found->Send<&MyActor::GetValue>();
}
```

在分布式模式下，还可以指定节点 ID：

```cpp
auto remote = co_await ncaf::GetActorRefByName<MyActor>(/*node_id=*/1, "counter");
```

## 项目整体架构

从高层来看，`ncaf` 由以下几个核心模块组成：

- **公共 API 层（`include/ncaf/api.h`）**

  - 向外暴露统一的头文件，内部包含 Actor 注册表、调度器、网络与反射相关实现。
  - 提供全局函数：`Init` / `Shutdown` / `Spawn` / `DestroyActor` / `GetActorRefByName` / `ConfigureLogging` 等。
- **Actor 抽象层（`internal/actor.h` / `internal/actor_ref.h`）**

  - `internal::Actor<UserClass>`：封装单个 Actor 实例及其消息邮箱、调度器、生命周期管理。
  - `ActorMessage`：所有待执行消息的抽象基类，通过邮箱排队并由调度器拉取执行。
  - `ActorRef<UserClass>`：封装本地 / 远程 Actor 的引用，统一提供 `Send` / `SendLocal` 接口，并负责远程调用的协议封装与解包。
- **Actor 注册与全局运行时（`internal/actor_registry.h`）**

  - `internal::ActorRegistry`：负责 Actor 的创建、销毁和名字查找，可工作在单节点或分布式模式。
  - 全局默认注册表通过 `ncaf::Init` 创建，并由 `ncaf::Spawn` / `ncaf::DestroyActor` 等函数进行访问。
- **调度器与线程池（`internal/scheduler.h`）**

  - `WorkSharingThreadPool` / `PriorityThreadPool` / `WorkStealingThreadPool`：不同风格的线程池实现。
  - `SchedulerUnion`：将多个内层调度器组合成一个逻辑调度器，支持通过 `ActorConfig.scheduler_index` 精细路由。
- **网络与分布式通信（`internal/network.h`）**

  - `network::MessageBroker`：基于 ZeroMQ 实现跨节点请求 / 响应消息流与心跳检测。
  - `NodeInfo` 与 `HeartbeatConfig`：描述集群拓扑和心跳参数。
- **远程处理器注册与反射（`internal/remote_handler_registry.h` / `internal/reflect.h`）**

  - `RemoteActorRequestHandlerRegistry`：维护远程创建与远程方法调用的处理器集合。
  - `ncaf_remote(...)` 宏：在编译期为指定的工厂函数和成员方法生成远程调用处理器，并自动注册到全局表。
