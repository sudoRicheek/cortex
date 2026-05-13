// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#include "cortex_wire/metadata.hpp"

#include <msgpack.hpp>

#include <cstring>
#include <string>
#include <string_view>

namespace cortex_wire
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

// ---- MetadataBuilder ------------------------------------------------------

MetadataBuilder::MetadataBuilder(std::uint32_t field_count)
: sbuf_(), packer_(sbuf_)
{
  packer_.pack_array(field_count);
}

void MetadataBuilder::pack_nil() {packer_.pack_nil();}
void MetadataBuilder::pack_bool(bool v) {packer_.pack(v);}
void MetadataBuilder::pack_int(std::int64_t v) {packer_.pack(v);}
void MetadataBuilder::pack_uint(std::uint64_t v) {packer_.pack(v);}
void MetadataBuilder::pack_double(double v) {packer_.pack(v);}

void MetadataBuilder::pack_str(std::string_view s)
{
  packer_.pack_str(static_cast<std::uint32_t>(s.size()));
  packer_.pack_str_body(s.data(), static_cast<std::uint32_t>(s.size()));
}

void MetadataBuilder::pack_bin(const void * data, std::size_t size)
{
  packer_.pack_bin(static_cast<std::uint32_t>(size));
  packer_.pack_bin_body(static_cast<const char *>(data), static_cast<std::uint32_t>(size));
}

std::uint32_t MetadataBuilder::add_oob_buffer(const void * data, std::size_t size)
{
  const auto idx = static_cast<std::uint32_t>(oob_buffers_.size());
  const auto * p = static_cast<const std::uint8_t *>(data);
  oob_buffers_.emplace_back(p, p + size);
  return idx;
}

namespace
{

void pack_shape(msgpack::packer<msgpack::sbuffer> & pk, const std::vector<std::int64_t> & shape)
{
  pk.pack_array(static_cast<std::uint32_t>(shape.size()));
  for (auto d : shape) {
    if (d < 0) {
      pk.pack(d);
    } else {
      pk.pack(static_cast<std::uint64_t>(d));
    }
  }
}

void pack_str_key(msgpack::packer<msgpack::sbuffer> & pk, std::string_view key)
{
  pk.pack_str(static_cast<std::uint32_t>(key.size()));
  pk.pack_str_body(key.data(), static_cast<std::uint32_t>(key.size()));
}

void pack_str_pair(
  msgpack::packer<msgpack::sbuffer> & pk, std::string_view key, std::string_view value)
{
  pack_str_key(pk, key);
  pack_str_key(pk, value);
}

}  // namespace

void MetadataBuilder::pack_numpy_oob(
  std::string_view dtype, const std::vector<std::int64_t> & shape,
  const void * buffer_data, std::size_t buffer_size)
{
  const auto idx = add_oob_buffer(buffer_data, buffer_size);
  packer_.pack_map(4);
  pack_str_pair(packer_, "__cortex_oob__", "numpy");
  pack_str_key(packer_, "buffer");
  packer_.pack(idx);
  pack_str_pair(packer_, "dtype", dtype);
  pack_str_key(packer_, "shape");
  pack_shape(packer_, shape);
}

void MetadataBuilder::pack_torch_oob(
  std::string_view dtype, const std::vector<std::int64_t> & shape,
  std::string_view device, bool requires_grad,
  const void * buffer_data, std::size_t buffer_size)
{
  const auto idx = add_oob_buffer(buffer_data, buffer_size);
  packer_.pack_map(6);
  pack_str_pair(packer_, "__cortex_oob__", "torch");
  pack_str_key(packer_, "buffer");
  packer_.pack(idx);
  pack_str_pair(packer_, "dtype", dtype);
  pack_str_key(packer_, "shape");
  pack_shape(packer_, shape);
  pack_str_pair(packer_, "device", device);
  pack_str_key(packer_, "requires_grad");
  packer_.pack(requires_grad);
}

MetadataBuilder::Frames MetadataBuilder::finish() &&
{
  Frames out;
  out.metadata.assign(
    reinterpret_cast<const std::uint8_t *>(sbuf_.data()),
    reinterpret_cast<const std::uint8_t *>(sbuf_.data()) + sbuf_.size());
  out.oob_buffers = std::move(oob_buffers_);
  sbuf_.clear();
  return out;
}

}  // namespace cortex_wire
