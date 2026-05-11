// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#ifndef CORTEX_ROS2_BRIDGE__ADAPTERS__OOB_HELPERS_HPP_
#define CORTEX_ROS2_BRIDGE__ADAPTERS__OOB_HELPERS_HPP_

#include <cortex_wire/header.hpp>
#include <cortex_wire/metadata.hpp>
#include <cortex_wire/oob_buffer.hpp>

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace cortex_ros2_bridge::adapters
{

// Parse the byte width from a numpy dtype string like "<f4", "<u1", ">i8".
// Returns 0 if the string doesn't follow the {endian}{kind}{byte_count}
// pattern (e.g. complex/datetime types we don't support yet).
inline std::size_t dtype_size_bytes(std::string_view dtype)
{
  if (dtype.size() < 3) {return 0;}
  std::size_t n = 0;
  for (std::size_t i = 2; i < dtype.size(); ++i) {
    const char c = dtype[i];
    if (!std::isdigit(static_cast<unsigned char>(c))) {return 0;}
    n = n * 10 + static_cast<std::size_t>(c - '0');
  }
  return n;
}

// Compute total element count from a shape vector.
inline std::size_t product(const std::vector<std::int64_t> & shape)
{
  std::size_t n = 1;
  for (auto d : shape) {
    if (d < 0) {return 0;}
    n *= static_cast<std::size_t>(d);
  }
  return n;
}

// Look up an OOB descriptor under `field` and return a typed view over its
// underlying ZMQ frame. Throws WireDecodeError if the field isn't an OOB
// descriptor, the buffer index is out of range, or dtype/shape don't match
// the expected width.
template<typename T>
cortex_wire::OobBuffer<T> oob_view(
  const msgpack::object & field,
  const std::vector<cortex_wire::ZmqFramePtr> & frames,
  std::string_view adapter, std::string_view expected_dtype)
{
  auto desc = cortex_wire::DecodedMetadata::as_oob(field);
  if (!desc) {
    throw cortex_wire::WireDecodeError(
            std::string(adapter) + ": expected OOB descriptor field");
  }
  if (desc->dtype != expected_dtype) {
    throw cortex_wire::WireDecodeError(
            std::string(adapter) + ": dtype mismatch (got '" + desc->dtype +
            "', expected '" + std::string(expected_dtype) + "')");
  }
  if (desc->buffer_index >= frames.size()) {
    throw cortex_wire::WireDecodeError(
            std::string(adapter) + ": OOB buffer index " +
            std::to_string(desc->buffer_index) + " >= frames.size() " +
            std::to_string(frames.size()));
  }
  const auto & frame = frames[desc->buffer_index];
  const std::size_t expected_bytes = product(desc->shape) * sizeof(T);
  if (frame->size() < expected_bytes) {
    throw cortex_wire::WireDecodeError(
            std::string(adapter) + ": OOB frame too small (" +
            std::to_string(frame->size()) + " < " + std::to_string(expected_bytes) + ")");
  }
  return cortex_wire::OobBuffer<T>(frame, product(desc->shape));
}

// Like oob_view but also returns the shape; useful for adapters that need
// to inspect dimensions (e.g. ImageMessage validating H×W×C).
struct OobByteView
{
  cortex_wire::OobDescriptor descriptor;
  const std::uint8_t * data = nullptr;
  std::size_t size = 0;
  cortex_wire::ZmqFramePtr frame;
};

inline OobByteView oob_bytes(
  const msgpack::object & field,
  const std::vector<cortex_wire::ZmqFramePtr> & frames,
  std::string_view adapter)
{
  auto desc = cortex_wire::DecodedMetadata::as_oob(field);
  if (!desc) {
    throw cortex_wire::WireDecodeError(
            std::string(adapter) + ": expected OOB descriptor field");
  }
  if (desc->buffer_index >= frames.size()) {
    throw cortex_wire::WireDecodeError(
            std::string(adapter) + ": OOB buffer index out of range");
  }
  OobByteView out;
  out.descriptor = *desc;
  out.frame = frames[desc->buffer_index];
  out.data = static_cast<const std::uint8_t *>(out.frame->data());
  out.size = out.frame->size();
  return out;
}

// Pull a string field; throws on type mismatch.
inline std::string read_str_field(
  const cortex_wire::DecodedMetadata & m, std::size_t i, std::string_view adapter)
{
  const auto & f = m.field(i);
  if (f.type != msgpack::type::STR) {
    throw cortex_wire::WireDecodeError(
            std::string(adapter) + ": field[" + std::to_string(i) + "] expected str");
  }
  return std::string(f.via.str.ptr, f.via.str.size);
}

inline std::int64_t read_int_field(
  const cortex_wire::DecodedMetadata & m, std::size_t i, std::string_view adapter)
{
  const auto & f = m.field(i);
  if (f.type == msgpack::type::POSITIVE_INTEGER) {return static_cast<std::int64_t>(f.via.u64);}
  if (f.type == msgpack::type::NEGATIVE_INTEGER) {return f.via.i64;}
  throw cortex_wire::WireDecodeError(
          std::string(adapter) + ": field[" + std::to_string(i) + "] expected int");
}

inline bool is_nil_field(const cortex_wire::DecodedMetadata & m, std::size_t i)
{
  return m.field(i).type == msgpack::type::NIL;
}

inline void require_field_count(
  const cortex_wire::DecodedMetadata & m, std::size_t expected, std::string_view adapter)
{
  if (m.field_count() != expected) {
    throw cortex_wire::WireDecodeError(
            std::string(adapter) + ": expected " + std::to_string(expected) +
            " fields, got " + std::to_string(m.field_count()));
  }
}

}  // namespace cortex_ros2_bridge::adapters

#endif  // CORTEX_ROS2_BRIDGE__ADAPTERS__OOB_HELPERS_HPP_
