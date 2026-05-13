// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#include "cortex_ros2_bridge/adapters/arrays.hpp"
#include "cortex_ros2_bridge/adapters/oob_helpers.hpp"

#include <cortex_wire/metadata.hpp>

#include <cstring>
#include <memory>
#include <string>
#include <utility>

namespace cortex_ros2_bridge::adapters
{

namespace
{

// Common decode path for ArrayMessage. Returns (data_bytes_view, shape, name,
// frame_id) after validating field shape; the caller plugs the bytes into
// the typed MultiArray.
struct DecodedArray
{
  OobByteView data;
  std::vector<std::int64_t> shape;
  std::string name;
  std::string frame_id;
};

DecodedArray decode_array_metadata(
  const CortexInbound & in, std::string_view adapter, std::string_view expected_dtype)
{
  require_field_count(in.metadata, 3, adapter);
  auto bytes = oob_bytes(in.metadata.field(0), in.oob_frames, adapter);
  if (bytes.descriptor.dtype != expected_dtype) {
    throw cortex_wire::WireDecodeError(
            std::string(adapter) + ": dtype mismatch (got '" + bytes.descriptor.dtype +
            "', expected '" + std::string(expected_dtype) + "')");
  }
  DecodedArray d;
  d.data = bytes;
  d.shape = std::move(bytes.descriptor.shape);
  d.name = read_str_field(in.metadata, 1, adapter);
  d.frame_id = read_str_field(in.metadata, 2, adapter);
  return d;
}

// Populate a std_msgs/MultiArrayLayout from a shape vector. Strides count
// elements, not bytes (per std_msgs convention).
void fill_layout(
  std_msgs::msg::MultiArrayLayout & layout, const std::vector<std::int64_t> & shape)
{
  layout.dim.clear();
  layout.dim.reserve(shape.size());
  std::size_t total = 1;
  for (auto d : shape) {
    total *= static_cast<std::size_t>(d);
  }
  std::size_t stride = total;
  for (std::size_t i = 0; i < shape.size(); ++i) {
    std_msgs::msg::MultiArrayDimension dim;
    dim.label.clear();
    dim.size = static_cast<std::uint32_t>(shape[i]);
    dim.stride = static_cast<std::uint32_t>(stride);
    layout.dim.push_back(std::move(dim));
    if (dim.size > 0) {stride /= static_cast<std::size_t>(shape[i]);}
  }
  layout.data_offset = 0;
}

// Recover the logical shape from a MultiArray layout. Returns the shape;
// empty if layout has no dimensions (which is legal for a 1-D array — the
// caller falls back to data.size() in that case).
std::vector<std::int64_t> layout_to_shape(const std_msgs::msg::MultiArrayLayout & layout)
{
  std::vector<std::int64_t> shape;
  shape.reserve(layout.dim.size());
  for (const auto & d : layout.dim) {
    shape.push_back(static_cast<std::int64_t>(d.size));
  }
  return shape;
}

template<typename Elem>
void copy_oob_to_data(const OobByteView & oob, std::vector<Elem> & out)
{
  const std::size_t n = oob.size / sizeof(Elem);
  out.resize(n);
  std::memcpy(out.data(), oob.data, n * sizeof(Elem));
}

template<typename Elem>
CortexOutbound pack_array_outbound(
  std::string_view dtype,
  const std::vector<Elem> & data,
  const std::vector<std::int64_t> & shape,
  std::string_view name, std::string_view frame_id)
{
  cortex_wire::MetadataBuilder b(3);
  b.pack_numpy_oob(dtype, shape, data.data(), data.size() * sizeof(Elem));
  b.pack_str(name);
  b.pack_str(frame_id);
  auto frames = std::move(b).finish();
  return CortexOutbound{std::move(frames.metadata), std::move(frames.oob_buffers)};
}

}  // namespace

// ---- ArrayFloat32Adapter -------------------------------------------------

cortex_wire::MessageKind ArrayFloat32Adapter::cortex_kind() const
{
  return cortex_wire::MessageKind::ArrayMessage;
}
std::string_view ArrayFloat32Adapter::ros2_type_name() const
{
  return "std_msgs/msg/Float32MultiArray";
}

std::unique_ptr<std_msgs::msg::Float32MultiArray> ArrayFloat32Adapter::to_ros2(
  const CortexInbound & in) const
{
  auto d = decode_array_metadata(in, "ArrayFloat32Adapter", "<f4");
  auto out = std::make_unique<std_msgs::msg::Float32MultiArray>();
  fill_layout(out->layout, d.shape);
  copy_oob_to_data(d.data, out->data);
  return out;
}

CortexOutbound ArrayFloat32Adapter::to_cortex(
  const std_msgs::msg::Float32MultiArray & msg, std::uint64_t /*sequence*/,
  const BridgeEntry & /*cfg*/) const
{
  auto shape = layout_to_shape(msg.layout);
  if (shape.empty()) {shape.push_back(static_cast<std::int64_t>(msg.data.size()));}
  return pack_array_outbound<float>("<f4", msg.data, shape, "", "");
}

// ---- ArrayFloat64Adapter -------------------------------------------------

cortex_wire::MessageKind ArrayFloat64Adapter::cortex_kind() const
{
  return cortex_wire::MessageKind::ArrayMessage;
}
std::string_view ArrayFloat64Adapter::ros2_type_name() const
{
  return "std_msgs/msg/Float64MultiArray";
}

std::unique_ptr<std_msgs::msg::Float64MultiArray> ArrayFloat64Adapter::to_ros2(
  const CortexInbound & in) const
{
  auto d = decode_array_metadata(in, "ArrayFloat64Adapter", "<f8");
  auto out = std::make_unique<std_msgs::msg::Float64MultiArray>();
  fill_layout(out->layout, d.shape);
  copy_oob_to_data(d.data, out->data);
  return out;
}

CortexOutbound ArrayFloat64Adapter::to_cortex(
  const std_msgs::msg::Float64MultiArray & msg, std::uint64_t,
  const BridgeEntry &) const
{
  auto shape = layout_to_shape(msg.layout);
  if (shape.empty()) {shape.push_back(static_cast<std::int64_t>(msg.data.size()));}
  return pack_array_outbound<double>("<f8", msg.data, shape, "", "");
}

// ---- registration --------------------------------------------------------

std::size_t register_array_adapters(AdapterRegistry & reg)
{
  std::size_t added = 0;
  added += reg.register_bidirectional<std_msgs::msg::Float32MultiArray>(
    std::make_shared<ArrayFloat32Adapter>()) ? 2 : 0;
  added += reg.register_bidirectional<std_msgs::msg::Float64MultiArray>(
    std::make_shared<ArrayFloat64Adapter>()) ? 2 : 0;
  return added;
}

}  // namespace cortex_ros2_bridge::adapters
