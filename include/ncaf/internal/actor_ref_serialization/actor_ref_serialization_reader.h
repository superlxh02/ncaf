#pragma once

#include <rfl/capnproto.hpp>

#include "ncaf/internal/actor_ref_serialization/actor_ref_deserialization_info.h"

namespace rfl {
namespace capnproto {
// 适配actor_ref的reader，提供反序列化ActorRef所需的信息
class ActorRefReader : public Reader {
 public:
  const ncaf::internal::ActorRefDeserializationInfo& info;
  explicit ActorRefReader(const ncaf::internal::ActorRefDeserializationInfo& info) : info(info) {}

  template <class VariantType, class UnionReaderType>
  Result<VariantType> read_union(const InputUnionType& u) const noexcept {
    const auto opt_pair = identify_discriminant(u);
    if (!opt_pair) {
      return error("Could not get the discriminant.");
    }
    const auto& [field, disc] = *opt_pair;
    return UnionReaderType::read(*this, disc, InputVarType {u.val_.get(field)});
  }

  std::optional<std::pair<capnp::StructSchema::Field, size_t>> identify_discriminant(
      const InputUnionType& _union) const noexcept {
    size_t ix = 0;
    for (auto field : _union.val_.getSchema().getFields()) {
      if (_union.val_.has(field)) {
        return std::make_pair(field, ix);
      }
      ++ix;
    }
    return std::optional<std::pair<capnp::StructSchema::Field, size_t>>();
  }
};

// 重写了capnproto的read函数，增加了ActorRefDeserializationInfo参数，用于反序列化ActorRef时提供必要的信息
// 当反序列化的类型是ActorRef时，使用ActorRefReader进行反序列化，提供ActorRefDeserializationInfo信息
template <class T, class... Ps>
auto read(const InputVarType& _obj, const ncaf::internal::ActorRefDeserializationInfo& info) {
  const ActorRefReader r {info};
  return rfl::parsing::Parser<ActorRefReader, Writer, T, Processors<SnakeCaseToCamelCase, Ps...>>::read(r, _obj);
}

template <class T, class... Ps>
Result<internal::wrap_in_rfl_array_t<T>> read(const concepts::ByteLike auto* bytes, size_t size,
                                              const Schema<T>& schema,
                                              const ncaf::internal::ActorRefDeserializationInfo& info) {
  const auto array_ptr = kj::ArrayPtr<const kj::byte>(internal::ptr_cast<const kj::byte*>(bytes), size);
  auto input_stream = kj::ArrayInputStream(array_ptr);
  auto message_reader = capnp::PackedMessageReader(input_stream);
  const auto root_name = get_root_name<std::remove_cv_t<T>, Ps...>();
  const auto root_schema = schema.value().getNested(root_name.c_str());
  const auto input_var = InputVarType {message_reader.getRoot<capnp::DynamicStruct>(root_schema.asStruct())};
  return read<T, Ps...>(input_var, info);
}
}  // namespace capnproto

}  // namespace rfl
