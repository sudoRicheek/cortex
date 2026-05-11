// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#include "cortex_ros2_bridge/adapters/tensor.hpp"
#include "cortex_ros2_bridge/adapters/oob_helpers.hpp"

#include <cortex_wire/metadata.hpp>

#include <cstring>
#include <memory>
#include <utility>

namespace cortex_ros2_bridge::adapters
{

cortex_wire::MessageKind TensorFloat32Adapter::cortex_kind() const
{
  return cortex_wire::MessageKind::TensorMessage;
}
std::string_view TensorFloat32Adapter::ros2_type_name() const
{
  return "std_msgs/msg/Float32MultiArray";
}

std::unique_ptr<std_msgs::msg::Float32MultiArray> TensorFloat32Adapter::to_ros2(
  const CortexInbound & in) const
{
  require_field_count(in.metadata, 2, "TensorFloat32Adapter");
  auto oob = oob_bytes(in.metadata.field(0), in.oob_frames, "TensorFloat32Adapter");
  if (oob.descriptor.dtype != "<f4") {
    throw cortex_wire::WireDecodeError(
            "TensorFloat32Adapter: dtype mismatch (got '" + oob.descriptor.dtype +
            "', expected '<f4')");
  }
  auto out = std::make_unique<std_msgs::msg::Float32MultiArray>();
  out->layout.dim.clear();
  for (auto d : oob.descriptor.shape) {
    std_msgs::msg::MultiArrayDimension dim;
    dim.size = static_cast<std::uint32_t>(d);
    out->layout.dim.push_back(std::move(dim));
  }
  const std::size_t n = oob.size / sizeof(float);
  out->data.resize(n);
  std::memcpy(out->data.data(), oob.data, n * sizeof(float));
  return out;
}

CortexOutbound TensorFloat32Adapter::to_cortex(
  const std_msgs::msg::Float32MultiArray & msg, std::uint64_t /*sequence*/,
  const BridgeEntry & /*cfg*/) const
{
  std::vector<std::int64_t> shape;
  shape.reserve(msg.layout.dim.size());
  for (const auto & d : msg.layout.dim) {
    shape.push_back(static_cast<std::int64_t>(d.size));
  }
  if (shape.empty()) {shape.push_back(static_cast<std::int64_t>(msg.data.size()));}

  cortex_wire::MetadataBuilder b(2);
  b.pack_torch_oob(
    "<f4", shape, "cpu", /*requires_grad=*/false,
    msg.data.data(), msg.data.size() * sizeof(float));
  b.pack_str("");  // name
  auto frames = std::move(b).finish();
  return CortexOutbound{std::move(frames.metadata), std::move(frames.oob_buffers)};
}

std::size_t register_tensor_adapters(AdapterRegistry & reg)
{
  return reg.register_bidirectional<std_msgs::msg::Float32MultiArray>(
    std::make_shared<TensorFloat32Adapter>()) ? 2 : 0;
}

}  // namespace cortex_ros2_bridge::adapters
