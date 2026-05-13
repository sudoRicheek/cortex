// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#include "cortex_wire/header.hpp"

#include <cstring>

namespace cortex_wire
{

namespace
{

// Portable big-endian u64 helpers — we do not rely on htobe64 / endian.h to
// keep the package buildable outside Linux.

std::uint64_t read_be64(const std::uint8_t * p) noexcept
{
  return (static_cast<std::uint64_t>(p[0]) << 56) |
         (static_cast<std::uint64_t>(p[1]) << 48) |
         (static_cast<std::uint64_t>(p[2]) << 40) |
         (static_cast<std::uint64_t>(p[3]) << 32) |
         (static_cast<std::uint64_t>(p[4]) << 24) |
         (static_cast<std::uint64_t>(p[5]) << 16) |
         (static_cast<std::uint64_t>(p[6]) << 8) |
         (static_cast<std::uint64_t>(p[7]));
}

void write_be64(std::uint8_t * p, std::uint64_t v) noexcept
{
  p[0] = static_cast<std::uint8_t>(v >> 56);
  p[1] = static_cast<std::uint8_t>(v >> 48);
  p[2] = static_cast<std::uint8_t>(v >> 40);
  p[3] = static_cast<std::uint8_t>(v >> 32);
  p[4] = static_cast<std::uint8_t>(v >> 24);
  p[5] = static_cast<std::uint8_t>(v >> 16);
  p[6] = static_cast<std::uint8_t>(v >> 8);
  p[7] = static_cast<std::uint8_t>(v);
}

}  // namespace

MessageHeader MessageHeader::from_bytes(const void * data, std::size_t size)
{
  if (size < kSize) {
    throw WireDecodeError("MessageHeader: need 24 bytes, got fewer");
  }
  const auto * p = static_cast<const std::uint8_t *>(data);
  MessageHeader h;
  h.fingerprint = read_be64(p);
  h.timestamp_ns = read_be64(p + 8);
  h.sequence = read_be64(p + 16);
  return h;
}

void MessageHeader::to_bytes(void * out) const noexcept
{
  auto * p = static_cast<std::uint8_t *>(out);
  write_be64(p, fingerprint);
  write_be64(p + 8, timestamp_ns);
  write_be64(p + 16, sequence);
}

}  // namespace cortex_wire
