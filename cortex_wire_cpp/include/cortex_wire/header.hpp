// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#ifndef CORTEX_WIRE__HEADER_HPP_
#define CORTEX_WIRE__HEADER_HPP_

#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace cortex_wire
{

// Fixed 24-byte header prepended to every Cortex multipart message, matching
// cortex/messages/base.py:MessageHeader. Layout is big-endian:
//   offset 0  : fingerprint   u64
//   offset 8  : timestamp_ns  u64
//   offset 16 : sequence      u64
struct MessageHeader
{
  std::uint64_t fingerprint = 0;
  std::uint64_t timestamp_ns = 0;
  std::uint64_t sequence = 0;

  static constexpr std::size_t kSize = 24;

  // Decode 24 bytes into a MessageHeader. Throws std::invalid_argument if
  // `size` < kSize.
  static MessageHeader from_bytes(const void * data, std::size_t size);

  // Encode into 24 bytes starting at `out`. Caller guarantees `out` is at
  // least kSize bytes.
  void to_bytes(void * out) const noexcept;
};

class WireDecodeError : public std::runtime_error
{
public:
  using std::runtime_error::runtime_error;
};

}  // namespace cortex_wire

#endif  // CORTEX_WIRE__HEADER_HPP_
