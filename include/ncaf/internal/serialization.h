#pragma once

#include <concepts>
#include <cstdint>
#include <type_traits>
#include <vector>

#include <rfl/capnproto.hpp>
#include <spdlog/spdlog.h>

#include "ncaf/internal/actor_config.h"
#include "ncaf/internal/actor_ref_serialization/actor_ref_deserialization_info.h"
#include "ncaf/internal/actor_ref_serialization/actor_ref_serialization_reader.h"
#include "ncaf/internal/logging.h"
#include "ncaf/internal/reflect.h"

namespace ncaf::internal::serde {

// 获取类型T的capnproto schema，并进行缓存，避免重复生成schema带来的性能开销
template <class T>
static auto GetCachedSchema() {
  thread_local auto schema = rfl::capnproto::to_schema<T>();
  return schema;
}
// 将对象序列化为字节数组，使用capnproto进行序列化
template <class T>
std::vector<char> Serialize(const T& obj) {
  return rfl::capnproto::write(obj, GetCachedSchema<T>());
}

// 将字节数组反序列化为对象，使用capnproto进行反序列化
template <class T>
T Deserialize(const uint8_t* data, size_t size) {
  return rfl::capnproto::read<T>(data, size, GetCachedSchema<T>()).value();
}
// 将字节数组反序列化为对象，增加ActorRefDeserializationInfo参数，用于反序列化ActorRef时提供必要的信息
template <class T>
T Deserialize(const uint8_t* data, size_t size, const ActorRefDeserializationInfo& info) {
  return rfl::capnproto::read<T>(data, size, GetCachedSchema<T>(), info).value();
}
// 网络请求
enum class NetworkRequestType : uint8_t {
  kActorCreationRequest = 0,
  kActorMethodCallRequest,
  kActorLookUpRequest,
};
// 网络回复
enum class NetworkReplyType : uint8_t {
  kActorCreationReturn = 0,
  kActorCreationError,
  kActorMethodCallReturn,
  kActorMethodCallError,
  kActorLookUpReturn,
  kActorLookUpError,

};
// 创建actor相关参数
template <class Tuple>
struct ActorCreationArgs {
  ActorConfig actor_config;
  Tuple args_tuple;
};
// 创建远程actor错误信息
struct ActorCreationError {
  std::string error;
};
// 远程调用参数
template <class Tuple>
struct ActorMethodCallArgs {
  Tuple args_tuple;
};
// 远程调用返回值
template <class T>
struct ActorMethodReturnValue {
  T return_value;
};
// 远程调用错误信息
struct ActorMethodReturnError {
  std::string error;
};
// Actorf方的返回值
template <>
struct ActorMethodReturnValue<void> {};
// Actor查找请求
struct ActorLookUpRequest {
  std::string actor_name;
};

// 反序列化函数，根据是否是成员函数指针走不同的反序列化函数
template <auto kFn>
auto DeserializeFnArgs(const uint8_t* data, size_t size, const ActorRefDeserializationInfo& info) {
  using Sig = reflect::Signature<decltype(kFn)>;
  if constexpr (std::is_member_function_pointer_v<decltype(kFn)>) {
    return Deserialize<ActorMethodCallArgs<typename Sig::DecayedArgsTupleType>>(data, size, info);
  } else {
    return Deserialize<ActorCreationArgs<typename Sig::DecayedArgsTupleType>>(data, size, info);
  }
}

struct MemoryBuf : std::streambuf {
  MemoryBuf(char const* base, size_t size) {
    char* p(const_cast<char*>(base));
    this->setg(p, p, p + size);
  }
};
struct InputMemoryStream : virtual MemoryBuf, std::istream {
  InputMemoryStream(char const* base, size_t size)
      : MemoryBuf(base, size), std::istream(static_cast<std::streambuf*>(this)) {}
};

template <class B>
class BufferReader {
 public:
  explicit BufferReader(B buffer)
      : buffer_(std::move(buffer)), start_(static_cast<uint8_t*>(buffer_.data())), size_(buffer_.size()) {}

  template <class T>
    requires std::integral<T> || std::floating_point<T> || std::is_enum_v<T>
  T NextPrimitive() {
    NCAF_THROW_CHECK_LE(offset_ + sizeof(T), size_) << "Buffer overflow, offset: " << offset_ << ", size: " << size_;
    T value = *reinterpret_cast<const T*>(Current());
    offset_ += sizeof(T);
    return value;
  }

  std::string PullString(size_t length) {
    NCAF_THROW_CHECK_LE(offset_ + length, size_) << "Buffer overflow, offset: " << offset_ << ", size: " << size_;
    std::string value(reinterpret_cast<const char*>(Current()), length);
    offset_ += length;
    return value;
  }

  const uint8_t* Current() const { return start_ + offset_; }

  size_t RemainingSize() const { return size_ - offset_; }

  InputMemoryStream ToInputMemoryStream() const {
    return InputMemoryStream(reinterpret_cast<const char*>(Current()), RemainingSize());
  }

 private:
  B buffer_;
  const uint8_t* start_;
  size_t size_;
  size_t offset_ = 0;
};

template <class B>
class BufferWriter {
 public:
  explicit BufferWriter(B buffer)
      : buffer_(std::move(buffer)), start_(static_cast<uint8_t*>(buffer_.data())), size_(buffer_.size()) {}

  template <class T>
    requires std::integral<T> || std::floating_point<T> || std::is_enum_v<T>
  void WritePrimitive(T value) {
    NCAF_THROW_CHECK_LE(offset_ + sizeof(T), size_) << "Buffer overflow, offset: " << offset_ << ", size: " << size_;
    *reinterpret_cast<T*>(Current()) = value;
    offset_ += sizeof(T);
  }

  void CopyFrom(const uint8_t* data, size_t size) {
    NCAF_THROW_CHECK_LE(offset_ + size, size_)
        << "Buffer overflow, offset: " << offset_ << ", size: " << size_ << ", size_to_copy: " << size;
    std::memcpy(Current(), data, size);
    offset_ += size;
  }
  void CopyFrom(const char* data, size_t size) { CopyFrom(reinterpret_cast<const uint8_t*>(data), size); }

  const B& GetBuffer() const { return buffer_; }

  B&& MoveBufferOut() && { return std::move(buffer_); }

  bool EndReached() const { return offset_ == size_; }

 private:
  uint8_t* Current() { return start_ + offset_; }

  B buffer_;
  uint8_t* start_;
  size_t size_;
  size_t offset_ = 0;
};

inline std::string BufferToHex(const uint8_t* data, size_t size) {
  std::stringstream ss;
  for (size_t i = 0; i < size; i++) {
    ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]) << ' ';
  }
  return ss.str();
}
inline std::string BufferToHex(const char* data, size_t size) {
  return BufferToHex(reinterpret_cast<const uint8_t*>(data), size);
}
}  // namespace ncaf::internal::serde
