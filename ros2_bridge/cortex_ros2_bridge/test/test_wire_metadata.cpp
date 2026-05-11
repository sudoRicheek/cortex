// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#include "cortex_ros2_bridge/cortex_wire/metadata.hpp"

#include <gtest/gtest.h>
#include <msgpack.hpp>

#include <cstdint>
#include <string>
#include <vector>

using cortex_ros2_bridge::cortex_wire::DecodedMetadata;
using cortex_ros2_bridge::cortex_wire::OobDescriptor;
using cortex_ros2_bridge::cortex_wire::OobKind;
using cortex_ros2_bridge::cortex_wire::WireDecodeError;

namespace
{

// Build a metadata frame that mirrors what Cortex's
// serialize_message_frames() emits for an ImageMessage(data=ndarray,
// encoding="rgb8", width=640, height=480). Field order matches the dataclass
// declaration in cortex.messages.standard.ImageMessage:
//   data, encoding, width, height
std::vector<std::uint8_t> build_image_metadata()
{
  msgpack::sbuffer buf;
  msgpack::packer<msgpack::sbuffer> pk(buf);
  pk.pack_array(4);

  // Field 0: OOB numpy descriptor (data field, buffer index 0)
  pk.pack_map(4);
  pk.pack(std::string("__cortex_oob__")); pk.pack(std::string("numpy"));
  pk.pack(std::string("buffer"));         pk.pack(static_cast<std::uint32_t>(0));
  pk.pack(std::string("dtype"));          pk.pack(std::string("<u1"));
  pk.pack(std::string("shape"));
  pk.pack_array(3);
  pk.pack(static_cast<std::uint32_t>(480));
  pk.pack(static_cast<std::uint32_t>(640));
  pk.pack(static_cast<std::uint32_t>(3));

  // Field 1: encoding (str)
  pk.pack(std::string("rgb8"));
  // Field 2: width (u32)
  pk.pack(static_cast<std::uint32_t>(640));
  // Field 3: height (u32)
  pk.pack(static_cast<std::uint32_t>(480));

  return std::vector<std::uint8_t>(
    reinterpret_cast<const std::uint8_t *>(buf.data()),
    reinterpret_cast<const std::uint8_t *>(buf.data()) + buf.size());
}

}  // namespace

TEST(DecodedMetadata, ParsesImageLikeMetadata)
{
  const auto bytes = build_image_metadata();
  const auto meta = DecodedMetadata::from_bytes(bytes.data(), bytes.size());

  ASSERT_EQ(meta.field_count(), 4u);

  // Field 0: OOB descriptor
  const auto oob_opt = DecodedMetadata::as_oob(meta.field(0));
  ASSERT_TRUE(oob_opt.has_value());
  EXPECT_EQ(oob_opt->kind, OobKind::Numpy);
  EXPECT_EQ(oob_opt->buffer_index, 0u);
  EXPECT_EQ(oob_opt->dtype, "<u1");
  ASSERT_EQ(oob_opt->shape.size(), 3u);
  EXPECT_EQ(oob_opt->shape[0], 480);
  EXPECT_EQ(oob_opt->shape[1], 640);
  EXPECT_EQ(oob_opt->shape[2], 3);

  // Field 1: encoding
  ASSERT_EQ(meta.field(1).type, msgpack::type::STR);
  EXPECT_EQ(
    std::string(meta.field(1).via.str.ptr, meta.field(1).via.str.size), "rgb8");

  // Field 2,3: width/height
  ASSERT_EQ(meta.field(2).type, msgpack::type::POSITIVE_INTEGER);
  EXPECT_EQ(meta.field(2).via.u64, 640u);
  ASSERT_EQ(meta.field(3).type, msgpack::type::POSITIVE_INTEGER);
  EXPECT_EQ(meta.field(3).via.u64, 480u);
}

TEST(DecodedMetadata, NonOobMapsAreNotOob)
{
  // A regular user dict {"foo": 1} must not be misread as an OOB descriptor.
  msgpack::sbuffer buf;
  msgpack::packer<msgpack::sbuffer> pk(buf);
  pk.pack_array(1);
  pk.pack_map(1);
  pk.pack(std::string("foo"));
  pk.pack(static_cast<std::uint32_t>(1));

  const auto meta = DecodedMetadata::from_bytes(buf.data(), buf.size());
  ASSERT_EQ(meta.field_count(), 1u);
  EXPECT_FALSE(DecodedMetadata::as_oob(meta.field(0)).has_value());
}

TEST(DecodedMetadata, TorchOobDescriptor)
{
  msgpack::sbuffer buf;
  msgpack::packer<msgpack::sbuffer> pk(buf);
  pk.pack_array(1);
  pk.pack_map(6);
  pk.pack(std::string("__cortex_oob__")); pk.pack(std::string("torch"));
  pk.pack(std::string("buffer"));         pk.pack(static_cast<std::uint32_t>(2));
  pk.pack(std::string("dtype"));          pk.pack(std::string("<f4"));
  pk.pack(std::string("shape"));
  pk.pack_array(2);
  pk.pack(static_cast<std::uint32_t>(64));
  pk.pack(static_cast<std::uint32_t>(64));
  pk.pack(std::string("device"));         pk.pack(std::string("cuda:0"));
  pk.pack(std::string("requires_grad"));  pk.pack(true);

  const auto meta = DecodedMetadata::from_bytes(buf.data(), buf.size());
  const auto oob = DecodedMetadata::as_oob(meta.field(0));
  ASSERT_TRUE(oob.has_value());
  EXPECT_EQ(oob->kind, OobKind::Torch);
  EXPECT_EQ(oob->buffer_index, 2u);
  EXPECT_EQ(oob->dtype, "<f4");
  EXPECT_EQ(oob->shape, (std::vector<std::int64_t>{64, 64}));
  EXPECT_EQ(oob->device, "cuda:0");
  EXPECT_TRUE(oob->requires_grad);
}

TEST(DecodedMetadata, NonArrayRootRejected)
{
  msgpack::sbuffer buf;
  msgpack::packer<msgpack::sbuffer> pk(buf);
  pk.pack_map(1);
  pk.pack(std::string("a"));
  pk.pack(1);
  EXPECT_THROW(DecodedMetadata::from_bytes(buf.data(), buf.size()), WireDecodeError);
}

TEST(DecodedMetadata, OutOfRangeAccessThrows)
{
  msgpack::sbuffer buf;
  msgpack::packer<msgpack::sbuffer> pk(buf);
  pk.pack_array(1);
  pk.pack(42);
  const auto meta = DecodedMetadata::from_bytes(buf.data(), buf.size());
  EXPECT_NO_THROW((void)meta.field(0));
  EXPECT_THROW((void)meta.field(1), WireDecodeError);
}

TEST(DecodedMetadata, MalformedBytesThrow)
{
  const std::uint8_t junk[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  EXPECT_THROW(DecodedMetadata::from_bytes(junk, sizeof(junk)), WireDecodeError);
}

TEST(DecodedMetadata, UnknownOobKindThrows)
{
  msgpack::sbuffer buf;
  msgpack::packer<msgpack::sbuffer> pk(buf);
  pk.pack_array(1);
  pk.pack_map(2);
  pk.pack(std::string("__cortex_oob__")); pk.pack(std::string("alien"));
  pk.pack(std::string("buffer"));         pk.pack(static_cast<std::uint32_t>(0));

  const auto meta = DecodedMetadata::from_bytes(buf.data(), buf.size());
  EXPECT_THROW((void)DecodedMetadata::as_oob(meta.field(0)), WireDecodeError);
}

TEST(DecodedMetadata, ForwardCompatibleUnknownKeys)
{
  // A descriptor with an unknown extra key must still parse cleanly — we
  // want to roll out new fields on the Python side without breaking older
  // bridge builds.
  msgpack::sbuffer buf;
  msgpack::packer<msgpack::sbuffer> pk(buf);
  pk.pack_array(1);
  pk.pack_map(5);
  pk.pack(std::string("__cortex_oob__")); pk.pack(std::string("numpy"));
  pk.pack(std::string("buffer"));         pk.pack(static_cast<std::uint32_t>(0));
  pk.pack(std::string("dtype"));          pk.pack(std::string("<f8"));
  pk.pack(std::string("shape"));
  pk.pack_array(1);
  pk.pack(static_cast<std::uint32_t>(10));
  pk.pack(std::string("future_field"));   pk.pack(123);

  const auto meta = DecodedMetadata::from_bytes(buf.data(), buf.size());
  const auto oob = DecodedMetadata::as_oob(meta.field(0));
  ASSERT_TRUE(oob.has_value());
  EXPECT_EQ(oob->dtype, "<f8");
}
