// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#ifndef CORTEX_ROS2_BRIDGE__CORTEX_WIRE__METADATA_HPP_
#define CORTEX_ROS2_BRIDGE__CORTEX_WIRE__METADATA_HPP_

#include <msgpack.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cortex_ros2_bridge/cortex_wire/header.hpp"

namespace cortex_ros2_bridge::cortex_wire
{

enum class OobKind { Numpy, Torch };

// Mirror of the {"__cortex_oob__": "numpy", ...} / "torch" descriptor that
// cortex.utils.serialization._encode_transport_value emits. The `buffer`
// index refers into the OOB frames that follow the metadata frame on the
// wire (i.e. frame index = 2 + buffer_index in the multipart message).
struct OobDescriptor
{
  OobKind kind = OobKind::Numpy;
  std::uint32_t buffer_index = 0;
  std::string dtype;                 // numpy dtype string, e.g. "<f4", "<u1"
  std::vector<std::int64_t> shape;
  // Torch-only fields:
  std::string device;
  bool requires_grad = false;
};

// Owned msgpack object tree for a metadata frame. The unpacker zone is held
// internally so msgpack::object references stay valid for the lifetime of
// this object.
//
// Cortex's metadata frame is always a msgpack array of N field values in
// dataclass declaration order. OOB descriptors appear inline as map objects
// with a "__cortex_oob__" key; everything else is a primitive, nested map,
// or nested list.
class DecodedMetadata
{
public:
  // Parse a msgpack-packed metadata frame.
  // Throws WireDecodeError on malformed msgpack or non-array root.
  static DecodedMetadata from_bytes(const void * data, std::size_t size);

  // Number of top-level fields (i.e. size of the dataclass's field list).
  std::size_t field_count() const noexcept;

  // Access a field by declaration order index.
  // Throws WireDecodeError if i is out of range.
  const msgpack::object & field(std::size_t i) const;

  // If `obj` is an OOB descriptor map, return it parsed; else nullopt.
  static std::optional<OobDescriptor> as_oob(const msgpack::object & obj);

private:
  DecodedMetadata() = default;

  msgpack::object_handle handle_;
  const msgpack::object * root_array_ = nullptr;  // points into handle_'s zone
  std::size_t count_ = 0;
};

}  // namespace cortex_ros2_bridge::cortex_wire

#endif  // CORTEX_ROS2_BRIDGE__CORTEX_WIRE__METADATA_HPP_
