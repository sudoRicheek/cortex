// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#include "cortex_ros2_bridge/adapters/transform.hpp"
#include "cortex_ros2_bridge/adapters/oob_helpers.hpp"

#include <cortex_wire/metadata.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

namespace cortex_ros2_bridge::adapters
{

namespace
{

// Read a 4x4 matrix from an OOB descriptor. Accepts <f8 or <f4 and returns
// 16 doubles in row-major order.
std::array<double, 16> read_matrix_4x4(
  const msgpack::object & field,
  const std::vector<cortex_wire::ZmqFramePtr> & frames,
  std::string_view adapter)
{
  auto oob = oob_bytes(field, frames, adapter);
  if (oob.descriptor.shape.size() != 2 ||
    oob.descriptor.shape[0] != 4 || oob.descriptor.shape[1] != 4)
  {
    throw cortex_wire::WireDecodeError(
            std::string(adapter) + ": expected 4×4 matrix");
  }
  std::array<double, 16> out{};
  if (oob.descriptor.dtype == "<f8") {
    if (oob.size < 16 * sizeof(double)) {
      throw cortex_wire::WireDecodeError(std::string(adapter) + ": short OOB frame");
    }
    for (std::size_t i = 0; i < 16; ++i) {
      double v;
      std::memcpy(&v, oob.data + i * sizeof(double), sizeof(double));
      out[i] = v;
    }
  } else if (oob.descriptor.dtype == "<f4") {
    if (oob.size < 16 * sizeof(float)) {
      throw cortex_wire::WireDecodeError(std::string(adapter) + ": short OOB frame");
    }
    for (std::size_t i = 0; i < 16; ++i) {
      float v;
      std::memcpy(&v, oob.data + i * sizeof(float), sizeof(float));
      out[i] = static_cast<double>(v);
    }
  } else {
    throw cortex_wire::WireDecodeError(
            std::string(adapter) + ": matrix dtype must be <f8 or <f4, got '" +
            oob.descriptor.dtype + "'");
  }
  return out;
}

// Shepperd's method for rotation-matrix → quaternion. R is row-major 3x3
// indexed via R[r*3 + c]. Returns (qx, qy, qz, qw).
std::array<double, 4> rotation_to_quaternion(const std::array<double, 9> & R)
{
  const double trace = R[0] + R[4] + R[8];
  double qx, qy, qz, qw;
  if (trace > 0.0) {
    const double s = std::sqrt(trace + 1.0) * 2.0;
    qw = 0.25 * s;
    qx = (R[7] - R[5]) / s;
    qy = (R[2] - R[6]) / s;
    qz = (R[3] - R[1]) / s;
  } else if (R[0] > R[4] && R[0] > R[8]) {
    const double s = std::sqrt(1.0 + R[0] - R[4] - R[8]) * 2.0;
    qw = (R[7] - R[5]) / s;
    qx = 0.25 * s;
    qy = (R[1] + R[3]) / s;
    qz = (R[2] + R[6]) / s;
  } else if (R[4] > R[8]) {
    const double s = std::sqrt(1.0 + R[4] - R[0] - R[8]) * 2.0;
    qw = (R[2] - R[6]) / s;
    qx = (R[1] + R[3]) / s;
    qy = 0.25 * s;
    qz = (R[5] + R[7]) / s;
  } else {
    const double s = std::sqrt(1.0 + R[8] - R[0] - R[4]) * 2.0;
    qw = (R[3] - R[1]) / s;
    qx = (R[2] + R[6]) / s;
    qy = (R[5] + R[7]) / s;
    qz = 0.25 * s;
  }
  return {qx, qy, qz, qw};
}

// Quaternion → 3x3 row-major rotation matrix.
std::array<double, 9> quaternion_to_rotation(double qx, double qy, double qz, double qw)
{
  std::array<double, 9> R;
  R[0] = 1.0 - 2.0 * (qy * qy + qz * qz);
  R[1] = 2.0 * (qx * qy - qz * qw);
  R[2] = 2.0 * (qx * qz + qy * qw);
  R[3] = 2.0 * (qx * qy + qz * qw);
  R[4] = 1.0 - 2.0 * (qx * qx + qz * qz);
  R[5] = 2.0 * (qy * qz - qx * qw);
  R[6] = 2.0 * (qx * qz - qy * qw);
  R[7] = 2.0 * (qy * qz + qx * qw);
  R[8] = 1.0 - 2.0 * (qx * qx + qy * qy);
  return R;
}

}  // namespace

cortex_wire::MessageKind TransformAdapter::cortex_kind() const
{
  return cortex_wire::MessageKind::TransformMessage;
}
std::string_view TransformAdapter::ros2_type_name() const
{
  return "geometry_msgs/msg/TransformStamped";
}

std::unique_ptr<geometry_msgs::msg::TransformStamped> TransformAdapter::to_ros2(
  const CortexInbound & in) const
{
  require_field_count(in.metadata, 3, "TransformAdapter");
  const auto matrix = read_matrix_4x4(in.metadata.field(0), in.oob_frames, "TransformAdapter");
  const auto frame_id = read_str_field(in.metadata, 1, "TransformAdapter");
  const auto child_frame_id = read_str_field(in.metadata, 2, "TransformAdapter");

  auto out = std::make_unique<geometry_msgs::msg::TransformStamped>();
  out->header.stamp.sec = static_cast<std::int32_t>(in.header.timestamp_ns / 1'000'000'000ULL);
  out->header.stamp.nanosec = static_cast<std::uint32_t>(
    in.header.timestamp_ns % 1'000'000'000ULL);
  out->header.frame_id = in.cfg.ros2.frame_id.value_or(frame_id);
  out->child_frame_id = child_frame_id;
  // matrix is row-major 4x4; index [r*4 + c].
  out->transform.translation.x = matrix[0 * 4 + 3];
  out->transform.translation.y = matrix[1 * 4 + 3];
  out->transform.translation.z = matrix[2 * 4 + 3];

  std::array<double, 9> R{
    matrix[0], matrix[1], matrix[2],
    matrix[4], matrix[5], matrix[6],
    matrix[8], matrix[9], matrix[10]};
  const auto q = rotation_to_quaternion(R);
  out->transform.rotation.x = q[0];
  out->transform.rotation.y = q[1];
  out->transform.rotation.z = q[2];
  out->transform.rotation.w = q[3];
  return out;
}

CortexOutbound TransformAdapter::to_cortex(
  const geometry_msgs::msg::TransformStamped & msg, std::uint64_t /*sequence*/,
  const BridgeEntry & /*cfg*/) const
{
  const auto R = quaternion_to_rotation(
    msg.transform.rotation.x, msg.transform.rotation.y,
    msg.transform.rotation.z, msg.transform.rotation.w);

  std::array<double, 16> matrix{};
  matrix[0] = R[0]; matrix[1] = R[1]; matrix[2] = R[2];
  matrix[4] = R[3]; matrix[5] = R[4]; matrix[6] = R[5];
  matrix[8] = R[6]; matrix[9] = R[7]; matrix[10] = R[8];
  matrix[3] = msg.transform.translation.x;
  matrix[7] = msg.transform.translation.y;
  matrix[11] = msg.transform.translation.z;
  matrix[15] = 1.0;

  cortex_wire::MetadataBuilder b(3);
  b.pack_numpy_oob("<f8", {4, 4}, matrix.data(), matrix.size() * sizeof(double));
  b.pack_str(msg.header.frame_id);
  b.pack_str(msg.child_frame_id);
  auto frames = std::move(b).finish();
  return CortexOutbound{std::move(frames.metadata), std::move(frames.oob_buffers)};
}

std::size_t register_transform_adapters(AdapterRegistry & reg)
{
  return reg.register_bidirectional<geometry_msgs::msg::TransformStamped>(
    std::make_shared<TransformAdapter>()) ? 2 : 0;
}

}  // namespace cortex_ros2_bridge::adapters
