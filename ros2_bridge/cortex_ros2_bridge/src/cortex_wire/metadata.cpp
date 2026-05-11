// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#include "cortex_ros2_bridge/cortex_wire/metadata.hpp"

#include <msgpack.hpp>

#include <cstring>
#include <string>
#include <string_view>

namespace cortex_ros2_bridge::cortex_wire
{

namespace
{

constexpr std::string_view kOobMarker = "__cortex_oob__";

std::string_view str_view(const msgpack::object & o)
{
  return std::string_view(o.via.str.ptr, o.via.str.size);
}

}  // namespace

DecodedMetadata DecodedMetadata::from_bytes(const void * data, std::size_t size)
{
  DecodedMetadata m;
  try {
    m.handle_ = msgpack::unpack(static_cast<const char *>(data), size);
  } catch (const msgpack::unpack_error & e) {
    throw WireDecodeError(std::string("metadata frame: msgpack error: ") + e.what());
  } catch (const std::exception & e) {
    throw WireDecodeError(std::string("metadata frame: parse failed: ") + e.what());
  }

  const msgpack::object & root = m.handle_.get();
  if (root.type != msgpack::type::ARRAY) {
    throw WireDecodeError(
      "metadata frame: expected msgpack array at root (Cortex packs field values in "
      "declaration order)");
  }
  m.root_array_ = &root;
  m.count_ = root.via.array.size;
  return m;
}

std::size_t DecodedMetadata::field_count() const noexcept
{
  return count_;
}

const msgpack::object & DecodedMetadata::field(std::size_t i) const
{
  if (!root_array_ || i >= count_) {
    throw WireDecodeError(
      "metadata frame: field index " + std::to_string(i) + " out of range (count=" +
      std::to_string(count_) + ")");
  }
  return root_array_->via.array.ptr[i];
}

std::optional<OobDescriptor> DecodedMetadata::as_oob(const msgpack::object & obj)
{
  if (obj.type != msgpack::type::MAP) {
    return std::nullopt;
  }

  // Walk the map looking for the marker key; bail early if not found. We do
  // not assume a key order, but we expect the marker key to be one of the
  // first entries (Python emits it first).
  const msgpack::object_kv * entries = obj.via.map.ptr;
  const std::uint32_t n = obj.via.map.size;

  bool is_oob = false;
  OobDescriptor desc;

  for (std::uint32_t i = 0; i < n; ++i) {
    const auto & k = entries[i].key;
    const auto & v = entries[i].val;
    if (k.type != msgpack::type::STR) {
      // Cortex always uses string keys in OOB descriptors; mixed keys mean
      // this map is a plain user dict.
      return std::nullopt;
    }
    const auto key = str_view(k);

    if (key == kOobMarker) {
      if (v.type != msgpack::type::STR) {
        return std::nullopt;
      }
      const auto kind_str = str_view(v);
      if (kind_str == "numpy") {
        desc.kind = OobKind::Numpy;
      } else if (kind_str == "torch") {
        desc.kind = OobKind::Torch;
      } else {
        // Unknown variant — refuse to misinterpret it as a user dict.
        throw WireDecodeError(
          std::string("OOB descriptor: unknown kind '") + std::string(kind_str) + "'");
      }
      is_oob = true;
    } else if (key == "buffer") {
      if (v.type == msgpack::type::POSITIVE_INTEGER) {
        desc.buffer_index = static_cast<std::uint32_t>(v.via.u64);
      } else if (v.type == msgpack::type::NEGATIVE_INTEGER) {
        throw WireDecodeError("OOB descriptor: 'buffer' must be non-negative");
      }
    } else if (key == "dtype") {
      if (v.type != msgpack::type::STR) {
        throw WireDecodeError("OOB descriptor: 'dtype' must be a string");
      }
      desc.dtype = std::string(str_view(v));
    } else if (key == "shape") {
      if (v.type != msgpack::type::ARRAY) {
        throw WireDecodeError("OOB descriptor: 'shape' must be an array");
      }
      desc.shape.clear();
      desc.shape.reserve(v.via.array.size);
      for (std::uint32_t j = 0; j < v.via.array.size; ++j) {
        const auto & dim = v.via.array.ptr[j];
        if (dim.type == msgpack::type::POSITIVE_INTEGER) {
          desc.shape.push_back(static_cast<std::int64_t>(dim.via.u64));
        } else if (dim.type == msgpack::type::NEGATIVE_INTEGER) {
          desc.shape.push_back(dim.via.i64);
        } else {
          throw WireDecodeError("OOB descriptor: 'shape' entries must be integers");
        }
      }
    } else if (key == "device") {
      if (v.type == msgpack::type::STR) {
        desc.device = std::string(str_view(v));
      }
    } else if (key == "requires_grad") {
      if (v.type == msgpack::type::BOOLEAN) {
        desc.requires_grad = v.via.boolean;
      }
    }
    // Unknown keys are tolerated — forward compatibility with future descriptor
    // fields.
  }

  if (!is_oob) {
    return std::nullopt;
  }
  return desc;
}

}  // namespace cortex_ros2_bridge::cortex_wire
