// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#include "cortex_ros2_bridge/adapters/primitives.hpp"
#include "cortex_ros2_bridge/registry.hpp"

#include <cortex_wire/metadata.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <vector>

using cortex_ros2_bridge::AdapterRegistry;
using cortex_ros2_bridge::BridgeEntry;
using cortex_ros2_bridge::CortexInbound;
using cortex_ros2_bridge::adapters::BytesAdapter;
using cortex_ros2_bridge::adapters::FloatAdapter;
using cortex_ros2_bridge::adapters::HeaderAdapter;
using cortex_ros2_bridge::adapters::IntAdapter;
using cortex_ros2_bridge::adapters::StringAdapter;
using cortex_ros2_bridge::adapters::TimestampAdapter;
using cortex_wire::DecodedMetadata;
using cortex_wire::MessageHeader;
using cortex_wire::MessageKind;

namespace
{

// Build a CortexInbound from a metadata byte buffer for tests. The frame
// references the caller-owned bytes; keep them alive past the call.
struct InboundFixture
{
  std::vector<std::uint8_t> metadata_bytes;
  cortex_wire::DecodedMetadata metadata;
  std::vector<cortex_wire::ZmqFramePtr> oob_frames;
  cortex_wire::MessageHeader header{};
  BridgeEntry cfg;

  explicit InboundFixture(std::vector<std::uint8_t> bytes)
  : metadata_bytes(std::move(bytes)),
    metadata(DecodedMetadata::from_bytes(metadata_bytes.data(), metadata_bytes.size()))
  {
  }

  CortexInbound view() const {return CortexInbound{header, metadata, oob_frames, cfg};}
};

}  // namespace

// ---- StringAdapter --------------------------------------------------------

TEST(StringAdapter, CortexToRos2)
{
  cortex_wire::MetadataBuilder b(1);
  b.pack_str("hello cortex");
  auto frames = std::move(b).finish();
  InboundFixture fx(std::move(frames.metadata));

  StringAdapter adapter;
  auto msg = adapter.to_ros2(fx.view());
  ASSERT_NE(msg, nullptr);
  EXPECT_EQ(msg->data, "hello cortex");
}

TEST(StringAdapter, Ros2ToCortexRoundTrip)
{
  std_msgs::msg::String src;
  src.data = "round trip";

  StringAdapter adapter;
  BridgeEntry cfg;
  auto out = adapter.to_cortex(src, /*sequence=*/123, cfg);
  EXPECT_TRUE(out.oob_buffers.empty());

  InboundFixture fx(std::move(out.metadata));
  auto round = adapter.to_ros2(fx.view());
  ASSERT_NE(round, nullptr);
  EXPECT_EQ(round->data, "round trip");
}

TEST(StringAdapter, RejectsWrongFieldCount)
{
  cortex_wire::MetadataBuilder b(2);
  b.pack_str("a");
  b.pack_str("b");
  auto frames = std::move(b).finish();
  InboundFixture fx(std::move(frames.metadata));

  StringAdapter adapter;
  EXPECT_THROW(adapter.to_ros2(fx.view()), cortex_wire::WireDecodeError);
}

TEST(StringAdapter, KindAndTypeNames)
{
  StringAdapter a;
  EXPECT_EQ(a.cortex_kind(), MessageKind::StringMessage);
  EXPECT_EQ(a.ros2_type_name(), "std_msgs/msg/String");
}

// ---- IntAdapter -----------------------------------------------------------

TEST(IntAdapter, CortexToRos2WithNegativeInt)
{
  cortex_wire::MetadataBuilder b(1);
  b.pack_int(-42);
  auto frames = std::move(b).finish();
  InboundFixture fx(std::move(frames.metadata));

  IntAdapter adapter;
  auto msg = adapter.to_ros2(fx.view());
  EXPECT_EQ(msg->data, -42);
}

TEST(IntAdapter, RoundTrip)
{
  std_msgs::msg::Int64 src;
  src.data = 1'234'567'890LL;
  IntAdapter adapter;
  BridgeEntry cfg;
  auto out = adapter.to_cortex(src, 0, cfg);
  InboundFixture fx(std::move(out.metadata));
  EXPECT_EQ(adapter.to_ros2(fx.view())->data, 1'234'567'890LL);
}

// ---- FloatAdapter ---------------------------------------------------------

TEST(FloatAdapter, RoundTrip)
{
  std_msgs::msg::Float64 src;
  src.data = 2.718281828;
  FloatAdapter adapter;
  BridgeEntry cfg;
  auto out = adapter.to_cortex(src, 0, cfg);
  InboundFixture fx(std::move(out.metadata));
  EXPECT_DOUBLE_EQ(adapter.to_ros2(fx.view())->data, 2.718281828);
}

TEST(FloatAdapter, AcceptsIntegerInCortex)
{
  // Cortex may emit an integer where the dataclass declares float; tolerate.
  cortex_wire::MetadataBuilder b(1);
  b.pack_int(7);
  auto frames = std::move(b).finish();
  InboundFixture fx(std::move(frames.metadata));
  FloatAdapter adapter;
  EXPECT_DOUBLE_EQ(adapter.to_ros2(fx.view())->data, 7.0);
}

// ---- BytesAdapter ---------------------------------------------------------

TEST(BytesAdapter, RoundTripUsesMsgpackBin)
{
  std_msgs::msg::ByteMultiArray src;
  // std_msgs/ByteMultiArray::data is std::vector<unsigned char> (rosidl maps
  // byte[] to unsigned char), so we cast through that.
  src.data = std::vector<unsigned char>{0x00, 0x01, 0xfe, 0xff};
  BytesAdapter adapter;
  BridgeEntry cfg;
  auto out = adapter.to_cortex(src, 0, cfg);

  // Verify the metadata field is encoded as msgpack BIN, not STR.
  auto md = DecodedMetadata::from_bytes(out.metadata.data(), out.metadata.size());
  ASSERT_EQ(md.field_count(), 1u);
  EXPECT_EQ(md.field(0).type, msgpack::type::BIN);

  InboundFixture fx(std::move(out.metadata));
  auto round = adapter.to_ros2(fx.view());
  ASSERT_EQ(round->data.size(), 4u);
  EXPECT_EQ(static_cast<std::uint8_t>(round->data[0]), 0x00);
  EXPECT_EQ(static_cast<std::uint8_t>(round->data[2]), 0xfe);
  EXPECT_EQ(static_cast<std::uint8_t>(round->data[3]), 0xff);
}

// ---- TimestampAdapter -----------------------------------------------------

TEST(TimestampAdapter, RoundTrip)
{
  builtin_interfaces::msg::Time src;
  src.sec = 1715000000;
  src.nanosec = 123456789;

  TimestampAdapter adapter;
  BridgeEntry cfg;
  auto out = adapter.to_cortex(src, 0, cfg);
  InboundFixture fx(std::move(out.metadata));
  auto round = adapter.to_ros2(fx.view());
  EXPECT_EQ(round->sec, 1715000000);
  EXPECT_EQ(round->nanosec, 123456789u);
}

TEST(TimestampAdapter, RejectsBadFieldCount)
{
  cortex_wire::MetadataBuilder b(1);
  b.pack_int(0);
  auto frames = std::move(b).finish();
  InboundFixture fx(std::move(frames.metadata));
  TimestampAdapter adapter;
  EXPECT_THROW(adapter.to_ros2(fx.view()), cortex_wire::WireDecodeError);
}

// ---- HeaderAdapter --------------------------------------------------------

TEST(HeaderAdapter, RoundTrip)
{
  std_msgs::msg::Header src;
  src.stamp.sec = 100;
  src.stamp.nanosec = 200u;
  src.frame_id = "base_link";

  HeaderAdapter adapter;
  BridgeEntry cfg;
  auto out = adapter.to_cortex(src, /*sequence=*/55, cfg);

  // Cortex side carries 4 fields: stamp_sec, stamp_nanosec, frame_id, sequence.
  auto md = DecodedMetadata::from_bytes(out.metadata.data(), out.metadata.size());
  ASSERT_EQ(md.field_count(), 4u);

  InboundFixture fx(std::move(out.metadata));
  auto round = adapter.to_ros2(fx.view());
  EXPECT_EQ(round->stamp.sec, 100);
  EXPECT_EQ(round->stamp.nanosec, 200u);
  EXPECT_EQ(round->frame_id, "base_link");
}

TEST(HeaderAdapter, FrameIdYamlOverride)
{
  cortex_wire::MetadataBuilder b(4);
  b.pack_int(0);
  b.pack_uint(0);
  b.pack_str("sensor_frame");
  b.pack_uint(0);
  auto frames = std::move(b).finish();

  InboundFixture fx(std::move(frames.metadata));
  fx.cfg.ros2.frame_id = "map";  // YAML override

  HeaderAdapter adapter;
  auto out = adapter.to_ros2(fx.view());
  EXPECT_EQ(out->frame_id, "map") << "YAML frame_id must override the wire value";
}

// ---- registration helper --------------------------------------------------

TEST(RegisterPrimitives, RegistersAllSixBidirectional)
{
  AdapterRegistry reg;
  const auto added = cortex_ros2_bridge::adapters::register_primitives(reg);
  EXPECT_EQ(added, 12u);  // 6 adapters × 2 directions

  EXPECT_TRUE(reg.has_cortex_to_ros2(MessageKind::StringMessage, "std_msgs/msg/String"));
  EXPECT_TRUE(reg.has_cortex_to_ros2(MessageKind::IntMessage, "std_msgs/msg/Int64"));
  EXPECT_TRUE(reg.has_cortex_to_ros2(MessageKind::FloatMessage, "std_msgs/msg/Float64"));
  EXPECT_TRUE(reg.has_cortex_to_ros2(MessageKind::BytesMessage, "std_msgs/msg/ByteMultiArray"));
  EXPECT_TRUE(reg.has_cortex_to_ros2(MessageKind::TimestampMessage, "builtin_interfaces/msg/Time"));
  EXPECT_TRUE(reg.has_cortex_to_ros2(MessageKind::HeaderMessage, "std_msgs/msg/Header"));

  EXPECT_TRUE(reg.has_ros2_to_cortex(MessageKind::StringMessage, "std_msgs/msg/String"));
  EXPECT_TRUE(reg.has_ros2_to_cortex(MessageKind::HeaderMessage, "std_msgs/msg/Header"));
}

TEST(RegisterPrimitives, IsIdempotent)
{
  AdapterRegistry reg;
  cortex_ros2_bridge::adapters::register_primitives(reg);
  // Second call adds nothing.
  EXPECT_EQ(cortex_ros2_bridge::adapters::register_primitives(reg), 0u);
}
