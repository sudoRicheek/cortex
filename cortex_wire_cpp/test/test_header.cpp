// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#include "cortex_wire/header.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>

using cortex_wire::MessageHeader;
using cortex_wire::WireDecodeError;

TEST(MessageHeader, SizeIs24Bytes)
{
  EXPECT_EQ(MessageHeader::kSize, 24u);
}

TEST(MessageHeader, EncodeDecodeRoundTrip)
{
  MessageHeader h;
  h.fingerprint = 0xa51dd7f890942cadULL;  // ImageMessage from the catalogue
  h.timestamp_ns = 0x0123'4567'89ab'cdefULL;
  h.sequence = 42;

  std::array<std::uint8_t, MessageHeader::kSize> buf{};
  h.to_bytes(buf.data());

  const auto decoded = MessageHeader::from_bytes(buf.data(), buf.size());
  EXPECT_EQ(decoded.fingerprint, h.fingerprint);
  EXPECT_EQ(decoded.timestamp_ns, h.timestamp_ns);
  EXPECT_EQ(decoded.sequence, h.sequence);
}

TEST(MessageHeader, BigEndianByteLayout)
{
  // Spot-check that we agree with Python's struct.pack(">QQQ", ...). The
  // first byte of `fingerprint` must be its MSB.
  MessageHeader h;
  h.fingerprint = 0x0102030405060708ULL;
  h.timestamp_ns = 0x1112131415161718ULL;
  h.sequence = 0x2122232425262728ULL;

  std::array<std::uint8_t, 24> buf{};
  h.to_bytes(buf.data());

  const std::array<std::uint8_t, 24> expected = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
    0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
  };
  EXPECT_EQ(buf, expected);
}

TEST(MessageHeader, ShortBufferThrows)
{
  std::array<std::uint8_t, 23> buf{};
  EXPECT_THROW(MessageHeader::from_bytes(buf.data(), buf.size()), WireDecodeError);
}

TEST(MessageHeader, ZeroIsValid)
{
  std::array<std::uint8_t, 24> buf{};
  const auto h = MessageHeader::from_bytes(buf.data(), buf.size());
  EXPECT_EQ(h.fingerprint, 0u);
  EXPECT_EQ(h.timestamp_ns, 0u);
  EXPECT_EQ(h.sequence, 0u);
}

TEST(MessageHeader, ExtraBytesIgnored)
{
  // from_bytes only reads the first 24 bytes.
  std::array<std::uint8_t, 32> buf{};
  buf.fill(0xff);
  MessageHeader h;
  h.fingerprint = 7;
  h.timestamp_ns = 8;
  h.sequence = 9;
  h.to_bytes(buf.data());

  const auto decoded = MessageHeader::from_bytes(buf.data(), buf.size());
  EXPECT_EQ(decoded.fingerprint, 7u);
  EXPECT_EQ(decoded.timestamp_ns, 8u);
  EXPECT_EQ(decoded.sequence, 9u);
}
