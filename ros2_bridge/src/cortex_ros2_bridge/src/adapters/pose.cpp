// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#include "cortex_ros2_bridge/adapters/pose.hpp"
#include "cortex_ros2_bridge/adapters/oob_helpers.hpp"

#include <cortex_wire/metadata.hpp>

#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

namespace cortex_ros2_bridge::adapters
{

namespace
{

// Read a fixed-length float vector from an OOB descriptor. Accepts both
// <f8 (double) and <f4 (float); converts to double internally.
std::array<double, 4> read_fixed_floats(
  const msgpack::object & field,
  const std::vector<cortex_wire::ZmqFramePtr> & frames,
  std::size_t expected_len, std::string_view adapter)
{
  auto oob = oob_bytes(field, frames, adapter);
  if (product(oob.descriptor.shape) != expected_len) {
    throw cortex_wire::WireDecodeError(
            std::string(adapter) + ": expected " + std::to_string(expected_len) +
            " elements, got " + std::to_string(product(oob.descriptor.shape)));
  }
  std::array<double, 4> out{};
  if (oob.descriptor.dtype == "<f8") {
    if (oob.size < expected_len * sizeof(double)) {
      throw cortex_wire::WireDecodeError(std::string(adapter) + ": short OOB frame");
    }
    for (std::size_t i = 0; i < expected_len; ++i) {
      double v;
      std::memcpy(&v, oob.data + i * sizeof(double), sizeof(double));
      out[i] = v;
    }
  } else if (oob.descriptor.dtype == "<f4") {
    if (oob.size < expected_len * sizeof(float)) {
      throw cortex_wire::WireDecodeError(std::string(adapter) + ": short OOB frame");
    }
    for (std::size_t i = 0; i < expected_len; ++i) {
      float v;
      std::memcpy(&v, oob.data + i * sizeof(float), sizeof(float));
      out[i] = static_cast<double>(v);
    }
  } else {
    throw cortex_wire::WireDecodeError(
            std::string(adapter) + ": expected float dtype, got '" +
            oob.descriptor.dtype + "'");
  }
  return out;
}

}  // namespace

cortex_wire::MessageKind PoseAdapter::cortex_kind() const
{
  return cortex_wire::MessageKind::PoseMessage;
}
std::string_view PoseAdapter::ros2_type_name() const
{
  return "geometry_msgs/msg/PoseStamped";
}

std::unique_ptr<geometry_msgs::msg::PoseStamped> PoseAdapter::to_ros2(
  const CortexInbound & in) const
{
  require_field_count(in.metadata, 4, "PoseAdapter");
  const auto pos = read_fixed_floats(in.metadata.field(0), in.oob_frames, 3, "PoseAdapter");
  const auto orient = read_fixed_floats(in.metadata.field(1), in.oob_frames, 4, "PoseAdapter");
  const auto frame_id = read_str_field(in.metadata, 2, "PoseAdapter");
  // field(3) is child_frame_id — read but discard (no equivalent in PoseStamped).
  (void)read_str_field(in.metadata, 3, "PoseAdapter");

  auto out = std::make_unique<geometry_msgs::msg::PoseStamped>();
  out->header.stamp.sec = static_cast<std::int32_t>(in.header.timestamp_ns / 1'000'000'000ULL);
  out->header.stamp.nanosec = static_cast<std::uint32_t>(
    in.header.timestamp_ns % 1'000'000'000ULL);
  out->header.frame_id = in.cfg.ros2.frame_id.value_or(frame_id);
  out->pose.position.x = pos[0];
  out->pose.position.y = pos[1];
  out->pose.position.z = pos[2];
  out->pose.orientation.x = orient[0];
  out->pose.orientation.y = orient[1];
  out->pose.orientation.z = orient[2];
  out->pose.orientation.w = orient[3];
  return out;
}

CortexOutbound PoseAdapter::to_cortex(
  const geometry_msgs::msg::PoseStamped & msg, std::uint64_t /*sequence*/,
  const BridgeEntry & /*cfg*/) const
{
  std::array<double, 3> position{
    msg.pose.position.x, msg.pose.position.y, msg.pose.position.z};
  std::array<double, 4> orientation{
    msg.pose.orientation.x, msg.pose.orientation.y,
    msg.pose.orientation.z, msg.pose.orientation.w};

  cortex_wire::MetadataBuilder b(4);
  b.pack_numpy_oob("<f8", {3}, position.data(), position.size() * sizeof(double));
  b.pack_numpy_oob("<f8", {4}, orientation.data(), orientation.size() * sizeof(double));
  b.pack_str(msg.header.frame_id);
  b.pack_str("");  // child_frame_id — no equivalent in PoseStamped
  auto frames = std::move(b).finish();
  return CortexOutbound{std::move(frames.metadata), std::move(frames.oob_buffers)};
}

std::size_t register_pose_adapters(AdapterRegistry & reg)
{
  return reg.register_bidirectional<geometry_msgs::msg::PoseStamped>(
    std::make_shared<PoseAdapter>()) ? 2 : 0;
}

}  // namespace cortex_ros2_bridge::adapters
