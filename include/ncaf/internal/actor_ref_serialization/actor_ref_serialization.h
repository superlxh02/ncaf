#pragma once

#include "ncaf/internal/actor_ref.h"
#include "ncaf/internal/actor_ref_serialization/actor_ref_serialization_reader.h"

// 基于rlf的扩展，提供ActorRef的序列化和反序列化功能

namespace rfl {
template <typename U>
struct Reflector<ncaf::internal::ActorRef<U>> {
  struct ReflType {
    bool is_valid {};
    uint32_t node_id {};
    uint64_t actor_id {};
  };

  // rfl反序列化的时候会调用此接口,创建一个ActorRef对象，并设置本地运行时信息
  static ncaf::internal::ActorRef<U> to(const ReflType& rfl_type) noexcept {
    ncaf::internal::ActorRef<U> actor(0, rfl_type.node_id, rfl_type.actor_id, nullptr, nullptr);
    return actor;
  }
  // rfl序列化的时候会调用此接口
  static ReflType from(const ncaf::internal::ActorRef<U>& actor_ref) {
    return {
        .is_valid = actor_ref.is_empty_,
        .node_id = actor_ref.node_id_,
        .actor_id = actor_ref.actor_id_,
    };
  }
};

namespace parsing {
template <class ProcessorsType, class U>
struct Parser<capnproto::ActorRefReader, capnproto::Writer, ncaf::internal::ActorRef<U>, ProcessorsType> {
  using Reader = capnproto::ActorRefReader;
  using InputVarType = typename Reader::InputVarType;

  static Result<ncaf::internal::ActorRef<U>> read(const Reader& reader, const InputVarType& var) noexcept {
    using Type = typename Reflector<ncaf::internal::ActorRef<U>>::ReflType;
    auto actor_ref =
        Parser<capnproto::Reader, capnproto::Writer, ncaf::internal::ActorRef<U>, ProcessorsType>::read(reader, var)
            .value();
    const auto& info = reader.info;
    actor_ref.SetLocalRuntimeInfo(info.this_node_id, info.actor_look_up_fn(actor_ref.GetActorId()),
                                  info.message_broker);
    return actor_ref;
  }

  template <class Parent>
  static void write(const capnproto::Writer& w, const ncaf::internal::ActorRef<U>& ref, const Parent& parent) {
    Parser<capnproto::Reader, capnproto::Writer, ncaf::internal::ActorRef<U>, ProcessorsType>::write(w, ref, parent);
  }
};

}  // namespace parsing

}  // namespace rfl
