// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#include "cortex_ros2_bridge/adapters/primitives.hpp"

#include <cortex_wire/metadata.hpp>

#include <stdexcept>
#include <string>
#include <utility>

namespace cortex_ros2_bridge::adapters
{

namespace
{

// ---- decode helpers ------------------------------------------------------

[[noreturn]] void throw_field_type(
  std::string_view adapter, std::size_t field_index, std::string_view expected,
  const msgpack::object & got)
{
  throw cortex_wire::WireDecodeError(
    std::string(adapter) + ": field[" + std::to_string(field_index) +
    "] expected " + std::string(expected) + ", got type " +
    std::to_string(static_cast<int>(got.type)));
}

std::string read_str(
  const cortex_wire::DecodedMetadata & m, std::size_t i, std::string_view adapter)
{
  const auto & f = m.field(i);
  if (f.type != msgpack::type::STR) {
    throw_field_type(adapter, i, "str", f);
  }
  return std::string(f.via.str.ptr, f.via.str.size);
}

std::int64_t read_int(
  const cortex_wire::DecodedMetadata & m, std::size_t i, std::string_view adapter)
{
  const auto & f = m.field(i);
  if (f.type == msgpack::type::POSITIVE_INTEGER) {
    return static_cast<std::int64_t>(f.via.u64);
  }
  if (f.type == msgpack::type::NEGATIVE_INTEGER) {
    return f.via.i64;
  }
  throw_field_type(adapter, i, "int", f);
}

double read_double(
  const cortex_wire::DecodedMetadata & m, std::size_t i, std::string_view adapter)
{
  const auto & f = m.field(i);
  if (f.type == msgpack::type::FLOAT64 || f.type == msgpack::type::FLOAT32) {
    return f.via.f64;
  }
  // Tolerate Python ints landing on a float field — Cortex emits whatever
  // the user passed; coerce so we don't break on the (float=3) case.
  if (f.type == msgpack::type::POSITIVE_INTEGER) {
    return static_cast<double>(f.via.u64);
  }
  if (f.type == msgpack::type::NEGATIVE_INTEGER) {
    return static_cast<double>(f.via.i64);
  }
  throw_field_type(adapter, i, "float", f);
}

std::vector<std::uint8_t> read_bin(
  const cortex_wire::DecodedMetadata & m, std::size_t i, std::string_view adapter)
{
  const auto & f = m.field(i);
  if (f.type == msgpack::type::BIN) {
    return std::vector<std::uint8_t>(
      f.via.bin.ptr, f.via.bin.ptr + f.via.bin.size);
  }
  // BytesMessage may be packed as STR on some round-trips; tolerate it.
  if (f.type == msgpack::type::STR) {
    return std::vector<std::uint8_t>(
      f.via.str.ptr, f.via.str.ptr + f.via.str.size);
  }
  throw_field_type(adapter, i, "bin", f);
}

void check_field_count(
  const cortex_wire::DecodedMetadata & m, std::size_t expected, std::string_view adapter)
{
  if (m.field_count() != expected) {
    throw cortex_wire::WireDecodeError(
      std::string(adapter) + ": expected " + std::to_string(expected) +
      " fields, got " + std::to_string(m.field_count()));
  }
}

}  // namespace

// ---- StringAdapter ----  StringMessage(data: str)

cortex_wire::MessageKind StringAdapter::cortex_kind() const
{
  return cortex_wire::MessageKind::StringMessage;
}
std::string_view StringAdapter::ros2_type_name() const
{
  return "std_msgs/msg/String";
}

std::unique_ptr<std_msgs::msg::String> StringAdapter::to_ros2(const CortexInbound & in) const
{
  check_field_count(in.metadata, 1, "StringAdapter");
  auto out = std::make_unique<std_msgs::msg::String>();
  out->data = read_str(in.metadata, 0, "StringAdapter");
  return out;
}

CortexOutbound StringAdapter::to_cortex(
  const std_msgs::msg::String & msg, std::uint64_t /*sequence*/,
  const BridgeEntry & /*cfg*/) const
{
  cortex_wire::MetadataBuilder b(1);
  b.pack_str(msg.data);
  auto frames = std::move(b).finish();
  return CortexOutbound{std::move(frames.metadata), std::move(frames.oob_buffers)};
}

// ---- IntAdapter ----  IntMessage(data: int)

cortex_wire::MessageKind IntAdapter::cortex_kind() const
{
  return cortex_wire::MessageKind::IntMessage;
}
std::string_view IntAdapter::ros2_type_name() const {return "std_msgs/msg/Int64";}

std::unique_ptr<std_msgs::msg::Int64> IntAdapter::to_ros2(const CortexInbound & in) const
{
  check_field_count(in.metadata, 1, "IntAdapter");
  auto out = std::make_unique<std_msgs::msg::Int64>();
  out->data = read_int(in.metadata, 0, "IntAdapter");
  return out;
}

CortexOutbound IntAdapter::to_cortex(
  const std_msgs::msg::Int64 & msg, std::uint64_t, const BridgeEntry &) const
{
  cortex_wire::MetadataBuilder b(1);
  b.pack_int(msg.data);
  auto frames = std::move(b).finish();
  return CortexOutbound{std::move(frames.metadata), std::move(frames.oob_buffers)};
}

// ---- FloatAdapter ----  FloatMessage(data: float)

cortex_wire::MessageKind FloatAdapter::cortex_kind() const
{
  return cortex_wire::MessageKind::FloatMessage;
}
std::string_view FloatAdapter::ros2_type_name() const {return "std_msgs/msg/Float64";}

std::unique_ptr<std_msgs::msg::Float64> FloatAdapter::to_ros2(const CortexInbound & in) const
{
  check_field_count(in.metadata, 1, "FloatAdapter");
  auto out = std::make_unique<std_msgs::msg::Float64>();
  out->data = read_double(in.metadata, 0, "FloatAdapter");
  return out;
}

CortexOutbound FloatAdapter::to_cortex(
  const std_msgs::msg::Float64 & msg, std::uint64_t, const BridgeEntry &) const
{
  cortex_wire::MetadataBuilder b(1);
  b.pack_double(msg.data);
  auto frames = std::move(b).finish();
  return CortexOutbound{std::move(frames.metadata), std::move(frames.oob_buffers)};
}

// ---- BytesAdapter ----  BytesMessage(data: bytes) <-> std_msgs/ByteMultiArray

cortex_wire::MessageKind BytesAdapter::cortex_kind() const
{
  return cortex_wire::MessageKind::BytesMessage;
}
std::string_view BytesAdapter::ros2_type_name() const {return "std_msgs/msg/ByteMultiArray";}

std::unique_ptr<std_msgs::msg::ByteMultiArray> BytesAdapter::to_ros2(
  const CortexInbound & in) const
{
  check_field_count(in.metadata, 1, "BytesAdapter");
  auto out = std::make_unique<std_msgs::msg::ByteMultiArray>();
  // std_msgs/ByteMultiArray uses signed bytes (int8) for data.
  auto bin = read_bin(in.metadata, 0, "BytesAdapter");
  out->data.reserve(bin.size());
  for (auto b : bin) {
    out->data.push_back(static_cast<signed char>(b));
  }
  out->layout.data_offset = 0;
  return out;
}

CortexOutbound BytesAdapter::to_cortex(
  const std_msgs::msg::ByteMultiArray & msg, std::uint64_t, const BridgeEntry &) const
{
  cortex_wire::MetadataBuilder b(1);
  b.pack_bin(msg.data.data(), msg.data.size());
  auto frames = std::move(b).finish();
  return CortexOutbound{std::move(frames.metadata), std::move(frames.oob_buffers)};
}

// ---- TimestampAdapter ----  TimestampMessage(sec: int, nanosec: int)

cortex_wire::MessageKind TimestampAdapter::cortex_kind() const
{
  return cortex_wire::MessageKind::TimestampMessage;
}
std::string_view TimestampAdapter::ros2_type_name() const
{
  return "builtin_interfaces/msg/Time";
}

std::unique_ptr<builtin_interfaces::msg::Time> TimestampAdapter::to_ros2(
  const CortexInbound & in) const
{
  check_field_count(in.metadata, 2, "TimestampAdapter");
  auto out = std::make_unique<builtin_interfaces::msg::Time>();
  out->sec = static_cast<std::int32_t>(read_int(in.metadata, 0, "TimestampAdapter"));
  out->nanosec = static_cast<std::uint32_t>(read_int(in.metadata, 1, "TimestampAdapter"));
  return out;
}

CortexOutbound TimestampAdapter::to_cortex(
  const builtin_interfaces::msg::Time & msg, std::uint64_t, const BridgeEntry &) const
{
  cortex_wire::MetadataBuilder b(2);
  b.pack_int(msg.sec);
  b.pack_uint(msg.nanosec);
  auto frames = std::move(b).finish();
  return CortexOutbound{std::move(frames.metadata), std::move(frames.oob_buffers)};
}

// ---- HeaderAdapter ----  HeaderMessage(stamp_sec, stamp_nanosec, frame_id, sequence)

cortex_wire::MessageKind HeaderAdapter::cortex_kind() const
{
  return cortex_wire::MessageKind::HeaderMessage;
}
std::string_view HeaderAdapter::ros2_type_name() const {return "std_msgs/msg/Header";}

std::unique_ptr<std_msgs::msg::Header> HeaderAdapter::to_ros2(const CortexInbound & in) const
{
  check_field_count(in.metadata, 4, "HeaderAdapter");
  auto out = std::make_unique<std_msgs::msg::Header>();
  out->stamp.sec = static_cast<std::int32_t>(read_int(in.metadata, 0, "HeaderAdapter"));
  out->stamp.nanosec = static_cast<std::uint32_t>(read_int(in.metadata, 1, "HeaderAdapter"));
  out->frame_id = read_str(in.metadata, 2, "HeaderAdapter");
  // std_msgs/Header has no sequence field (it was removed in ROS 2). The
  // Cortex `sequence` field is dropped on the forward path; the YAML config
  // can attach a separate sequence-tracking node if needed. The reverse
  // direction always emits 0 to keep the schema balanced.
  (void)read_int(in.metadata, 3, "HeaderAdapter");
  // Apply YAML frame_id override if provided.
  if (in.cfg.ros2.frame_id) {
    out->frame_id = *in.cfg.ros2.frame_id;
  }
  return out;
}

CortexOutbound HeaderAdapter::to_cortex(
  const std_msgs::msg::Header & msg, std::uint64_t sequence, const BridgeEntry &) const
{
  cortex_wire::MetadataBuilder b(4);
  b.pack_int(msg.stamp.sec);
  b.pack_uint(msg.stamp.nanosec);
  b.pack_str(msg.frame_id);
  b.pack_uint(sequence);
  auto frames = std::move(b).finish();
  return CortexOutbound{std::move(frames.metadata), std::move(frames.oob_buffers)};
}

// ---- registration --------------------------------------------------------

std::size_t register_primitives(AdapterRegistry & registry)
{
  std::size_t added = 0;
  added += registry.register_bidirectional<std_msgs::msg::String>(
    std::make_shared<StringAdapter>()) ? 2 : 0;
  added += registry.register_bidirectional<std_msgs::msg::Int64>(
    std::make_shared<IntAdapter>()) ? 2 : 0;
  added += registry.register_bidirectional<std_msgs::msg::Float64>(
    std::make_shared<FloatAdapter>()) ? 2 : 0;
  added += registry.register_bidirectional<std_msgs::msg::ByteMultiArray>(
    std::make_shared<BytesAdapter>()) ? 2 : 0;
  added += registry.register_bidirectional<builtin_interfaces::msg::Time>(
    std::make_shared<TimestampAdapter>()) ? 2 : 0;
  added += registry.register_bidirectional<std_msgs::msg::Header>(
    std::make_shared<HeaderAdapter>()) ? 2 : 0;
  return added;
}

}  // namespace cortex_ros2_bridge::adapters
