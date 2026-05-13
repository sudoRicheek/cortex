// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#include "cortex_wire/metadata.hpp"

#include <gtest/gtest.h>
#include <msgpack.hpp>

#include <cstdint>
#include <string>
#include <vector>

using cortex_wire::DecodedMetadata;
using cortex_wire::MetadataBuilder;
using cortex_wire::OobDescriptor;
using cortex_wire::OobKind;
using cortex_wire::WireDecodeError;

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

// ---- MetadataBuilder + walk_oob -----------------------------------------

TEST(MetadataBuilder, PrimitivesRoundTrip)
{
  MetadataBuilder b(5);
  b.pack_str("hello");
  b.pack_int(-7);
  b.pack_uint(42);
  b.pack_double(3.14);
  b.pack_bool(true);
  auto frames = std::move(b).finish();
  ASSERT_TRUE(frames.oob_buffers.empty());

  const auto meta = DecodedMetadata::from_bytes(
    frames.metadata.data(), frames.metadata.size());
  ASSERT_EQ(meta.field_count(), 5u);

  ASSERT_EQ(meta.field(0).type, msgpack::type::STR);
  EXPECT_EQ(
    std::string(meta.field(0).via.str.ptr, meta.field(0).via.str.size), "hello");

  ASSERT_EQ(meta.field(1).type, msgpack::type::NEGATIVE_INTEGER);
  EXPECT_EQ(meta.field(1).via.i64, -7);

  ASSERT_EQ(meta.field(2).type, msgpack::type::POSITIVE_INTEGER);
  EXPECT_EQ(meta.field(2).via.u64, 42u);

  ASSERT_EQ(meta.field(3).type, msgpack::type::FLOAT64);
  EXPECT_DOUBLE_EQ(meta.field(3).via.f64, 3.14);

  ASSERT_EQ(meta.field(4).type, msgpack::type::BOOLEAN);
  EXPECT_TRUE(meta.field(4).via.boolean);
}

TEST(MetadataBuilder, BinDistinctFromStr)
{
  // BytesMessage's data must be packed as msgpack BIN, not STR — this is
  // the distinction Python's msgpack(use_bin_type=True) preserves and which
  // PR2 flagged as a gap.
  const std::uint8_t bytes[] = {0xfe, 0xed, 0xfa, 0xce};
  MetadataBuilder b(1);
  b.pack_bin(bytes, sizeof(bytes));
  auto frames = std::move(b).finish();

  const auto meta = DecodedMetadata::from_bytes(
    frames.metadata.data(), frames.metadata.size());
  ASSERT_EQ(meta.field_count(), 1u);
  ASSERT_EQ(meta.field(0).type, msgpack::type::BIN);
  ASSERT_EQ(meta.field(0).via.bin.size, sizeof(bytes));
  EXPECT_EQ(std::memcmp(meta.field(0).via.bin.ptr, bytes, sizeof(bytes)), 0);
}

TEST(MetadataBuilder, NumpyOobRoundTripsThroughAsOob)
{
  const std::vector<std::uint8_t> pixels(640 * 480 * 3, 0xa5);
  MetadataBuilder b(1);
  b.pack_numpy_oob("<u1", {480, 640, 3}, pixels.data(), pixels.size());
  auto frames = std::move(b).finish();
  ASSERT_EQ(frames.oob_buffers.size(), 1u);
  EXPECT_EQ(frames.oob_buffers[0].size(), pixels.size());
  EXPECT_EQ(frames.oob_buffers[0][0], 0xa5);

  const auto meta = DecodedMetadata::from_bytes(
    frames.metadata.data(), frames.metadata.size());
  const auto oob = DecodedMetadata::as_oob(meta.field(0));
  ASSERT_TRUE(oob.has_value());
  EXPECT_EQ(oob->kind, OobKind::Numpy);
  EXPECT_EQ(oob->buffer_index, 0u);
  EXPECT_EQ(oob->dtype, "<u1");
  EXPECT_EQ(oob->shape, (std::vector<std::int64_t>{480, 640, 3}));
}

TEST(MetadataBuilder, TorchOobCarriesDeviceAndGrad)
{
  const std::vector<float> weights(8, 0.5f);
  MetadataBuilder b(1);
  b.pack_torch_oob(
    "<f4", {2, 4}, "cuda:0", /*requires_grad=*/true,
    weights.data(), weights.size() * sizeof(float));
  auto frames = std::move(b).finish();

  const auto meta = DecodedMetadata::from_bytes(
    frames.metadata.data(), frames.metadata.size());
  const auto oob = DecodedMetadata::as_oob(meta.field(0));
  ASSERT_TRUE(oob.has_value());
  EXPECT_EQ(oob->kind, OobKind::Torch);
  EXPECT_EQ(oob->device, "cuda:0");
  EXPECT_TRUE(oob->requires_grad);
}

TEST(MetadataBuilder, LowLevelPackerForNestedStructures)
{
  // DictMessage adapter path: emit a nested {string: numpy_array} map by
  // hand using the low-level packer + add_oob_buffer interface.
  MetadataBuilder b(1);
  auto & pk = b.packer();
  pk.pack_map(2);
  // entry 1: "weights" -> numpy array (buffer 0)
  pk.pack_str(7); pk.pack_str_body("weights", 7);
  const std::vector<float> w(4, 1.0f);
  const auto buf_idx = b.add_oob_buffer(w.data(), w.size() * sizeof(float));
  pk.pack_map(4);
  pk.pack_str(14); pk.pack_str_body("__cortex_oob__", 14);
  pk.pack_str(5);  pk.pack_str_body("numpy", 5);
  pk.pack_str(6);  pk.pack_str_body("buffer", 6);
  pk.pack(buf_idx);
  pk.pack_str(5);  pk.pack_str_body("dtype", 5);
  pk.pack_str(3);  pk.pack_str_body("<f4", 3);
  pk.pack_str(5);  pk.pack_str_body("shape", 5);
  pk.pack_array(1); pk.pack(static_cast<std::uint32_t>(4));
  // entry 2: "scale" -> 2.0
  pk.pack_str(5); pk.pack_str_body("scale", 5);
  pk.pack(2.0);

  auto frames = std::move(b).finish();
  ASSERT_EQ(frames.oob_buffers.size(), 1u);

  // Now decode and use walk_oob to discover the nested descriptor.
  const auto meta = DecodedMetadata::from_bytes(
    frames.metadata.data(), frames.metadata.size());
  std::vector<OobDescriptor> found;
  DecodedMetadata::walk_oob(meta.field(0), [&](const OobDescriptor & d) {
    found.push_back(d);
  });
  ASSERT_EQ(found.size(), 1u);
  EXPECT_EQ(found[0].kind, OobKind::Numpy);
  EXPECT_EQ(found[0].buffer_index, 0u);
  EXPECT_EQ(found[0].dtype, "<f4");
}

TEST(MetadataBuilder, WalkOobRecursesIntoLists)
{
  // ListMessage adapter path: top-level field is a list-of-arrays.
  MetadataBuilder b(1);
  auto & pk = b.packer();
  pk.pack_array(2);
  for (int i = 0; i < 2; ++i) {
    const std::vector<std::uint8_t> chunk(8, static_cast<std::uint8_t>(i));
    const auto idx = b.add_oob_buffer(chunk.data(), chunk.size());
    pk.pack_map(4);
    pk.pack_str(14); pk.pack_str_body("__cortex_oob__", 14);
    pk.pack_str(5);  pk.pack_str_body("numpy", 5);
    pk.pack_str(6);  pk.pack_str_body("buffer", 6);
    pk.pack(idx);
    pk.pack_str(5);  pk.pack_str_body("dtype", 5);
    pk.pack_str(3);  pk.pack_str_body("<u1", 3);
    pk.pack_str(5);  pk.pack_str_body("shape", 5);
    pk.pack_array(1); pk.pack(static_cast<std::uint32_t>(8));
  }
  auto frames = std::move(b).finish();
  ASSERT_EQ(frames.oob_buffers.size(), 2u);

  const auto meta = DecodedMetadata::from_bytes(
    frames.metadata.data(), frames.metadata.size());
  std::vector<std::uint32_t> indexes;
  DecodedMetadata::walk_oob(meta.field(0), [&](const OobDescriptor & d) {
    indexes.push_back(d.buffer_index);
  });
  ASSERT_EQ(indexes.size(), 2u);
  EXPECT_EQ(indexes[0], 0u);
  EXPECT_EQ(indexes[1], 1u);
}

TEST(MetadataBuilder, WalkOobIgnoresPlainMaps)
{
  // A regular {"foo": 1, "bar": "baz"} dict must not be reported as OOB.
  MetadataBuilder b(1);
  auto & pk = b.packer();
  pk.pack_map(2);
  pk.pack_str(3); pk.pack_str_body("foo", 3);
  pk.pack(static_cast<std::uint32_t>(1));
  pk.pack_str(3); pk.pack_str_body("bar", 3);
  pk.pack_str(3); pk.pack_str_body("baz", 3);
  auto frames = std::move(b).finish();

  const auto meta = DecodedMetadata::from_bytes(
    frames.metadata.data(), frames.metadata.size());
  std::vector<OobDescriptor> found;
  DecodedMetadata::walk_oob(meta.field(0), [&](const OobDescriptor & d) {
    found.push_back(d);
  });
  EXPECT_TRUE(found.empty());
}
