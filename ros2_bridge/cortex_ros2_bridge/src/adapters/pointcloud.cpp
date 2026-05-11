// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#include "cortex_ros2_bridge/adapters/pointcloud.hpp"
#include "cortex_ros2_bridge/adapters/oob_helpers.hpp"

#include <cortex_wire/metadata.hpp>
#include <sensor_msgs/msg/point_field.hpp>

#include <cstring>
#include <memory>
#include <string>
#include <utility>

namespace cortex_ros2_bridge::adapters
{

namespace
{

// Reads an optional OOB field at metadata index `i`. Returns nullopt if the
// field is msgpack NIL; throws otherwise via oob_bytes() if malformed.
std::optional<OobByteView> optional_oob(
  const CortexInbound & in, std::size_t i, std::string_view adapter)
{
  if (is_nil_field(in.metadata, i)) {return std::nullopt;}
  return oob_bytes(in.metadata.field(i), in.oob_frames, adapter);
}

// One PointCloud2 PointField record.
sensor_msgs::msg::PointField make_field(
  const std::string & name, std::uint32_t offset, std::uint8_t datatype,
  std::uint32_t count)
{
  sensor_msgs::msg::PointField f;
  f.name = name;
  f.offset = offset;
  f.datatype = datatype;
  f.count = count;
  return f;
}

}  // namespace

cortex_wire::MessageKind PointCloudAdapter::cortex_kind() const
{
  return cortex_wire::MessageKind::PointCloudMessage;
}
std::string_view PointCloudAdapter::ros2_type_name() const
{
  return "sensor_msgs/msg/PointCloud2";
}

std::unique_ptr<sensor_msgs::msg::PointCloud2> PointCloudAdapter::to_ros2(
  const CortexInbound & in) const
{
  require_field_count(in.metadata, 5, "PointCloudAdapter");

  auto points = oob_bytes(in.metadata.field(0), in.oob_frames, "PointCloudAdapter");
  if (points.descriptor.dtype != "<f4") {
    throw cortex_wire::WireDecodeError(
            "PointCloudAdapter: 'points' must be float32 (<f4), got '" +
            points.descriptor.dtype + "'");
  }
  if (points.descriptor.shape.size() != 2 || points.descriptor.shape[1] != 3) {
    throw cortex_wire::WireDecodeError(
            "PointCloudAdapter: 'points' must be N×3");
  }
  const auto num_points = static_cast<std::uint32_t>(points.descriptor.shape[0]);

  auto colors_opt = optional_oob(in, 1, "PointCloudAdapter");
  auto intensity_opt = optional_oob(in, 2, "PointCloudAdapter");
  auto normals_opt = optional_oob(in, 3, "PointCloudAdapter");
  const auto frame_id = read_str_field(in.metadata, 4, "PointCloudAdapter");

  auto out = std::make_unique<sensor_msgs::msg::PointCloud2>();
  out->header.stamp.sec = static_cast<std::int32_t>(in.header.timestamp_ns / 1'000'000'000ULL);
  out->header.stamp.nanosec = static_cast<std::uint32_t>(
    in.header.timestamp_ns % 1'000'000'000ULL);
  out->header.frame_id = in.cfg.ros2.frame_id.value_or(frame_id);
  out->height = 1;
  out->width = num_points;
  out->is_bigendian = false;
  out->is_dense = true;

  // Build the field layout. Each point's record is concatenated in this order.
  std::uint32_t offset = 0;
  out->fields.push_back(make_field("x", offset, sensor_msgs::msg::PointField::FLOAT32, 1));
  offset += 4;
  out->fields.push_back(make_field("y", offset, sensor_msgs::msg::PointField::FLOAT32, 1));
  offset += 4;
  out->fields.push_back(make_field("z", offset, sensor_msgs::msg::PointField::FLOAT32, 1));
  offset += 4;

  if (colors_opt) {
    // RGB packed into a 4-byte field, padded — matches the PCL "rgb" convention.
    out->fields.push_back(make_field("rgb", offset, sensor_msgs::msg::PointField::UINT32, 1));
    offset += 4;
  }
  if (intensity_opt) {
    out->fields.push_back(
      make_field("intensity", offset, sensor_msgs::msg::PointField::FLOAT32, 1));
    offset += 4;
  }
  if (normals_opt) {
    out->fields.push_back(
      make_field("normal_x", offset, sensor_msgs::msg::PointField::FLOAT32, 1));
    offset += 4;
    out->fields.push_back(
      make_field("normal_y", offset, sensor_msgs::msg::PointField::FLOAT32, 1));
    offset += 4;
    out->fields.push_back(
      make_field("normal_z", offset, sensor_msgs::msg::PointField::FLOAT32, 1));
    offset += 4;
  }
  out->point_step = offset;
  out->row_step = out->point_step * out->width;

  out->data.resize(static_cast<std::size_t>(out->row_step));

  // Interleave the per-point records. We trust the OOB dtype/shape on
  // colors/intensity/normals; mismatches throw.
  if (colors_opt) {
    if (colors_opt->descriptor.dtype != "<u1" ||
      colors_opt->descriptor.shape.size() != 2 ||
      colors_opt->descriptor.shape[0] != num_points ||
      colors_opt->descriptor.shape[1] != 3)
    {
      throw cortex_wire::WireDecodeError(
              "PointCloudAdapter: 'colors' must be N×3 uint8");
    }
  }
  if (intensity_opt) {
    if (intensity_opt->descriptor.dtype != "<f4" ||
      product(intensity_opt->descriptor.shape) != num_points)
    {
      throw cortex_wire::WireDecodeError(
              "PointCloudAdapter: 'intensity' must be float32 with N elements");
    }
  }
  if (normals_opt) {
    if (normals_opt->descriptor.dtype != "<f4" ||
      normals_opt->descriptor.shape.size() != 2 ||
      normals_opt->descriptor.shape[0] != num_points ||
      normals_opt->descriptor.shape[1] != 3)
    {
      throw cortex_wire::WireDecodeError(
              "PointCloudAdapter: 'normals' must be N×3 float32");
    }
  }

  const auto * pts = points.data;
  const auto * cols = colors_opt ? colors_opt->data : nullptr;
  const auto * inten = intensity_opt ? intensity_opt->data : nullptr;
  const auto * nrm = normals_opt ? normals_opt->data : nullptr;
  for (std::uint32_t i = 0; i < num_points; ++i) {
    std::uint8_t * dst = out->data.data() + i * out->point_step;
    std::memcpy(dst, pts + i * 12, 12);   // x,y,z
    std::uint32_t o = 12;
    if (cols) {
      // rgb packed: r<<16 | g<<8 | b, stored as little-endian uint32.
      const auto r = cols[i * 3 + 0];
      const auto g = cols[i * 3 + 1];
      const auto b = cols[i * 3 + 2];
      const std::uint32_t packed =
        (static_cast<std::uint32_t>(r) << 16) |
        (static_cast<std::uint32_t>(g) << 8) |
        static_cast<std::uint32_t>(b);
      std::memcpy(dst + o, &packed, 4);
      o += 4;
    }
    if (inten) {
      std::memcpy(dst + o, inten + i * 4, 4);
      o += 4;
    }
    if (nrm) {
      std::memcpy(dst + o, nrm + i * 12, 12);
      o += 12;
    }
  }
  return out;
}

CortexOutbound PointCloudAdapter::to_cortex(
  const sensor_msgs::msg::PointCloud2 & msg, std::uint64_t /*sequence*/,
  const BridgeEntry & cfg) const
{
  const auto num_points = static_cast<std::uint32_t>(msg.width * msg.height);

  // Walk fields to find which channels are present and their offsets.
  std::optional<std::uint32_t> off_x, off_y, off_z, off_rgb, off_intensity;
  std::optional<std::uint32_t> off_nx, off_ny, off_nz;
  for (const auto & f : msg.fields) {
    if (f.name == "x") {off_x = f.offset;}
    else if (f.name == "y") {off_y = f.offset;}
    else if (f.name == "z") {off_z = f.offset;}
    else if (f.name == "rgb" || f.name == "rgba") {off_rgb = f.offset;}
    else if (f.name == "intensity") {off_intensity = f.offset;}
    else if (f.name == "normal_x") {off_nx = f.offset;}
    else if (f.name == "normal_y") {off_ny = f.offset;}
    else if (f.name == "normal_z") {off_nz = f.offset;}
  }
  if (!off_x || !off_y || !off_z) {
    throw cortex_wire::WireDecodeError(
            "PointCloudAdapter::to_cortex: PointCloud2 missing x/y/z fields");
  }

  const bool has_colors = off_rgb.has_value();
  const bool has_intensity = off_intensity.has_value();
  const bool has_normals = off_nx && off_ny && off_nz;

  std::vector<float> points(num_points * 3);
  std::vector<std::uint8_t> colors(has_colors ? num_points * 3 : 0);
  std::vector<float> intensity(has_intensity ? num_points : 0);
  std::vector<float> normals(has_normals ? num_points * 3 : 0);

  for (std::uint32_t i = 0; i < num_points; ++i) {
    const std::uint8_t * src = msg.data.data() + i * msg.point_step;
    std::memcpy(&points[i * 3 + 0], src + *off_x, 4);
    std::memcpy(&points[i * 3 + 1], src + *off_y, 4);
    std::memcpy(&points[i * 3 + 2], src + *off_z, 4);
    if (has_colors) {
      std::uint32_t packed;
      std::memcpy(&packed, src + *off_rgb, 4);
      colors[i * 3 + 0] = static_cast<std::uint8_t>((packed >> 16) & 0xff);
      colors[i * 3 + 1] = static_cast<std::uint8_t>((packed >> 8) & 0xff);
      colors[i * 3 + 2] = static_cast<std::uint8_t>(packed & 0xff);
    }
    if (has_intensity) {
      std::memcpy(&intensity[i], src + *off_intensity, 4);
    }
    if (has_normals) {
      std::memcpy(&normals[i * 3 + 0], src + *off_nx, 4);
      std::memcpy(&normals[i * 3 + 1], src + *off_ny, 4);
      std::memcpy(&normals[i * 3 + 2], src + *off_nz, 4);
    }
  }

  cortex_wire::MetadataBuilder b(5);
  b.pack_numpy_oob("<f4", {num_points, 3}, points.data(), points.size() * sizeof(float));
  if (has_colors) {
    b.pack_numpy_oob("<u1", {num_points, 3}, colors.data(), colors.size());
  } else {
    b.pack_nil();
  }
  if (has_intensity) {
    b.pack_numpy_oob("<f4", {num_points}, intensity.data(), intensity.size() * sizeof(float));
  } else {
    b.pack_nil();
  }
  if (has_normals) {
    b.pack_numpy_oob("<f4", {num_points, 3}, normals.data(), normals.size() * sizeof(float));
  } else {
    b.pack_nil();
  }
  // frame_id: prefer cfg override, else the ROS header.
  const std::string frame_id =
    cfg.ros2.frame_id.value_or(msg.header.frame_id);
  b.pack_str(frame_id);

  auto frames = std::move(b).finish();
  return CortexOutbound{std::move(frames.metadata), std::move(frames.oob_buffers)};
}

std::size_t register_pointcloud_adapters(AdapterRegistry & reg)
{
  return reg.register_bidirectional<sensor_msgs::msg::PointCloud2>(
    std::make_shared<PointCloudAdapter>()) ? 2 : 0;
}

}  // namespace cortex_ros2_bridge::adapters
