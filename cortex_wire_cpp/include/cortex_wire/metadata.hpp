// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#ifndef CORTEX_WIRE__METADATA_HPP_
#define CORTEX_WIRE__METADATA_HPP_

#include <msgpack.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cortex_wire/header.hpp"

namespace cortex_wire
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

  // Recursively walks `obj` and invokes `visitor(descriptor)` for every OOB
  // descriptor found, including those nested inside dict / list values.
  // Useful for DictMessage / ListMessage adapters that need to collect every
  // OOB buffer reachable from a top-level field.
  template <typename Fn>
  static void walk_oob(const msgpack::object & obj, Fn && visitor);

private:
  DecodedMetadata() = default;

  msgpack::object_handle handle_;
  const msgpack::object * root_array_ = nullptr;  // points into handle_'s zone
  std::size_t count_ = 0;
};

// Encoder for the metadata frame plus the variable-length list of OOB buffer
// frames. Mirrors cortex.utils.serialization.serialize_message_frames:
//   1. The caller packs `field_count` top-level values via pack_*().
//   2. Arrays are emitted via pack_numpy_oob() / pack_torch_oob() — the
//      builder writes a descriptor map inline and queues the raw buffer for
//      emission as a separate frame.
//   3. finish() returns the metadata bytes and the ordered OOB buffer list.
//
// For nested structures (Dict / List adapters), reach for packer() to write
// msgpack maps/arrays directly and use add_oob_buffer() to record buffers
// while emitting your own descriptor maps via packer().
class MetadataBuilder
{
public:
  // `field_count` is the number of top-level dataclass fields the caller
  // will pack. Packs the outer msgpack array header immediately.
  explicit MetadataBuilder(std::uint32_t field_count);

  // ---- top-level primitive packers ----
  void pack_nil();
  void pack_bool(bool v);
  void pack_int(std::int64_t v);
  void pack_uint(std::uint64_t v);
  void pack_double(double v);
  void pack_str(std::string_view s);
  void pack_bin(const void * data, std::size_t size);  // msgpack BIN, for `bytes`

  // ---- OOB array packers ----
  // Writes the descriptor map inline and records the raw buffer for emission
  // as the next OOB frame. Buffer bytes are copied into builder-owned storage.
  void pack_numpy_oob(
    std::string_view dtype,
    const std::vector<std::int64_t> & shape,
    const void * buffer_data, std::size_t buffer_size);

  void pack_torch_oob(
    std::string_view dtype,
    const std::vector<std::int64_t> & shape,
    std::string_view device,
    bool requires_grad,
    const void * buffer_data, std::size_t buffer_size);

  // ---- low-level access ----
  msgpack::packer<msgpack::sbuffer> & packer() noexcept {return packer_;}

  // Records a buffer and returns its 0-based index. Caller is responsible
  // for emitting the corresponding descriptor map via packer().
  std::uint32_t add_oob_buffer(const void * data, std::size_t size);

  // Finalize. Returns metadata bytes + OOB frames in emission order. The
  // builder is left in a moved-from state.
  struct Frames
  {
    std::vector<std::uint8_t> metadata;
    std::vector<std::vector<std::uint8_t>> oob_buffers;
  };
  Frames finish() &&;

private:
  msgpack::sbuffer sbuf_;
  msgpack::packer<msgpack::sbuffer> packer_;
  std::vector<std::vector<std::uint8_t>> oob_buffers_;
};

// ---- inline template definitions ----

template <typename Fn>
void DecodedMetadata::walk_oob(const msgpack::object & obj, Fn && visitor)
{
  if (auto desc = as_oob(obj)) {
    visitor(*desc);
    return;
  }
  if (obj.type == msgpack::type::MAP) {
    for (std::uint32_t i = 0; i < obj.via.map.size; ++i) {
      walk_oob(obj.via.map.ptr[i].val, std::forward<Fn>(visitor));
    }
  } else if (obj.type == msgpack::type::ARRAY) {
    for (std::uint32_t i = 0; i < obj.via.array.size; ++i) {
      walk_oob(obj.via.array.ptr[i], std::forward<Fn>(visitor));
    }
  }
}

}  // namespace cortex_wire

#endif  // CORTEX_WIRE__METADATA_HPP_
