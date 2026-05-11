// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#include "cortex_ros2_bridge/adapters/image.hpp"
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

// Channels per pixel for the encodings Cortex's ImageMessage commonly uses.
// Returns 0 for unknown encodings — the adapter then falls back to inferring
// from shape (H×W×C) and trusts the wire.
std::uint32_t channels_for_encoding(std::string_view enc)
{
  if (enc == "mono8" || enc == "mono16" || enc == "8UC1" || enc == "16UC1") {return 1;}
  if (enc == "rgb8" || enc == "bgr8" || enc == "8UC3" || enc == "16UC3") {return 3;}
  if (enc == "rgba8" || enc == "bgra8" || enc == "8UC4" || enc == "16UC4") {return 4;}
  return 0;
}

std::uint32_t bytes_per_pixel(std::string_view enc, std::uint32_t channels)
{
  if (enc == "mono16" || enc == "16UC1" || enc == "16UC3" || enc == "16UC4") {
    return channels * 2;
  }
  return channels;
}

}  // namespace

cortex_wire::MessageKind ImageAdapter::cortex_kind() const
{
  return cortex_wire::MessageKind::ImageMessage;
}
std::string_view ImageAdapter::ros2_type_name() const {return "sensor_msgs/msg/Image";}

std::unique_ptr<sensor_msgs::msg::Image> ImageAdapter::to_ros2(
  const CortexInbound & in) const
{
  require_field_count(in.metadata, 4, "ImageAdapter");

  auto oob = oob_bytes(in.metadata.field(0), in.oob_frames, "ImageAdapter");
  const auto encoding = read_str_field(in.metadata, 1, "ImageAdapter");
  const auto width = static_cast<std::uint32_t>(
    read_int_field(in.metadata, 2, "ImageAdapter"));
  const auto height = static_cast<std::uint32_t>(
    read_int_field(in.metadata, 3, "ImageAdapter"));

  auto out = std::make_unique<sensor_msgs::msg::Image>();
  out->header.stamp.sec = static_cast<std::int32_t>(in.header.timestamp_ns / 1'000'000'000ULL);
  out->header.stamp.nanosec = static_cast<std::uint32_t>(
    in.header.timestamp_ns % 1'000'000'000ULL);
  out->header.frame_id = in.cfg.ros2.frame_id.value_or("");
  out->encoding = encoding;
  out->is_bigendian = 0;
  out->width = width;
  out->height = height;

  std::uint32_t channels = channels_for_encoding(encoding);
  if (channels == 0 && oob.descriptor.shape.size() >= 3) {
    channels = static_cast<std::uint32_t>(oob.descriptor.shape[2]);
  }
  out->step = width * bytes_per_pixel(encoding, channels);

  // One memcpy from the OOB frame into the freshly-allocated Image::data.
  out->data.resize(oob.size);
  std::memcpy(out->data.data(), oob.data, oob.size);
  return out;
}

CortexOutbound ImageAdapter::to_cortex(
  const sensor_msgs::msg::Image & msg, std::uint64_t /*sequence*/,
  const BridgeEntry & /*cfg*/) const
{
  // Reconstruct an H×W or H×W×C shape that round-trips to the same
  // ImageMessage layout when decoded by Python cortex. We use the channel
  // hint from `step / width` (bytes per row / image width = bytes/pixel,
  // then / dtype_size = channels). 8-bit data assumed unless the encoding
  // string says otherwise.
  cortex_wire::MetadataBuilder b(4);

  const std::uint32_t bytes_per_row = msg.width > 0 ? msg.step : msg.width;
  const std::uint32_t bpp = msg.width > 0 ? bytes_per_row / msg.width : 0;
  const std::string dtype =
    (msg.encoding == "mono16" || msg.encoding.find("16U") != std::string::npos) ?
    "<u2" : "<u1";
  const std::uint32_t bytes_per_elem = (dtype == "<u2") ? 2 : 1;
  const std::uint32_t channels = bpp > 0 ? bpp / bytes_per_elem : 1;

  std::vector<std::int64_t> shape;
  shape.push_back(msg.height);
  shape.push_back(msg.width);
  if (channels > 1) {shape.push_back(channels);}

  b.pack_numpy_oob(dtype, shape, msg.data.data(), msg.data.size());
  b.pack_str(msg.encoding);
  b.pack_uint(msg.width);
  b.pack_uint(msg.height);
  auto frames = std::move(b).finish();
  return CortexOutbound{std::move(frames.metadata), std::move(frames.oob_buffers)};
}

std::size_t register_image_adapters(AdapterRegistry & reg)
{
  return reg.register_bidirectional<sensor_msgs::msg::Image>(
    std::make_shared<ImageAdapter>()) ? 2 : 0;
}

}  // namespace cortex_ros2_bridge::adapters
