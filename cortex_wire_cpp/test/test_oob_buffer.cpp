// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#include "cortex_wire/oob_buffer.hpp"

#include <gtest/gtest.h>
#include <zmq.hpp>

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

using cortex_wire::make_owned;
using cortex_wire::OobBuffer;
using cortex_wire::ZmqAllocator;
using cortex_wire::ZmqFramePtr;

namespace
{

ZmqFramePtr make_frame_with(const std::vector<std::uint8_t> & bytes)
{
  zmq::message_t msg(bytes.size());
  std::memcpy(msg.data(), bytes.data(), bytes.size());
  return make_owned(std::move(msg));
}

}  // namespace

TEST(OobBuffer, ReadsZmqFrameWithoutCopy)
{
  auto frame = make_frame_with({0, 1, 2, 3, 4, 5, 6, 7});
  const void * raw_data = frame->data();

  OobBuffer<std::uint8_t> view(frame, 8);
  EXPECT_EQ(view.size(), 8u);
  EXPECT_EQ(view.size_bytes(), 8u);
  EXPECT_FALSE(view.empty());

  // Pointer equality proves no copy occurred.
  EXPECT_EQ(reinterpret_cast<const void *>(view.data()), raw_data);

  // Element-wise content.
  for (std::size_t i = 0; i < 8; ++i) {
    EXPECT_EQ(view[i], static_cast<std::uint8_t>(i));
  }
}

TEST(OobBuffer, OffsetSlicing)
{
  auto frame = make_frame_with({10, 11, 12, 13, 14, 15});
  OobBuffer<std::uint8_t> view(frame, 3, /*byte_offset=*/2);
  ASSERT_EQ(view.size(), 3u);
  EXPECT_EQ(view[0], 12);
  EXPECT_EQ(view[1], 13);
  EXPECT_EQ(view[2], 14);
}

TEST(OobBuffer, OutlivesOriginalSharedPtr)
{
  // The buffer should still be readable after the only external shared_ptr
  // is dropped — OobBuffer owns its own reference.
  OobBuffer<std::uint8_t> view;
  {
    auto frame = make_frame_with({0xaa, 0xbb, 0xcc, 0xdd});
    view = OobBuffer<std::uint8_t>(frame, 4);
    EXPECT_EQ(frame.use_count(), 2);  // local + view
  }
  ASSERT_EQ(view.size(), 4u);
  EXPECT_EQ(view[0], 0xaa);
  EXPECT_EQ(view[3], 0xdd);
}

TEST(OobBuffer, DefaultConstructedIsEmpty)
{
  OobBuffer<std::uint32_t> empty;
  EXPECT_TRUE(empty.empty());
  EXPECT_EQ(empty.size(), 0u);
  EXPECT_EQ(empty.data(), nullptr);
  EXPECT_EQ(empty.begin(), empty.end());
}

TEST(OobBuffer, RangeIteration)
{
  auto frame = make_frame_with({1, 2, 3, 4, 5});
  OobBuffer<std::uint8_t> view(frame, 5);
  int sum = 0;
  for (auto v : view) {
    sum += v;
  }
  EXPECT_EQ(sum, 15);
}

TEST(ZmqAllocator, VectorStorageAliasesFrameBuffer)
{
  // The vector's storage pointer must equal the frame's data pointer — that
  // is the contract that lets us tie a std::vector's lifetime to a zmq
  // frame without allocating fresh memory. NOTE: std::vector's sizing ctor
  // value-initialises elements, so the frame's *content* is zeroed by this
  // constructor. The pointer equality is the zero-allocation proof; data
  // must be written into v.data() separately (e.g. via memcpy).
  auto frame = make_frame_with({0xde, 0xad, 0xbe, 0xef});
  void * frame_ptr = frame->data();

  ZmqAllocator<std::uint8_t> alloc(frame);
  std::vector<std::uint8_t, ZmqAllocator<std::uint8_t>> v(4, alloc);

  EXPECT_EQ(reinterpret_cast<void *>(v.data()), frame_ptr);
  EXPECT_EQ(v.size(), 4u);
}

TEST(ZmqAllocator, OverflowRequestThrows)
{
  auto frame = make_frame_with({1, 2});
  ZmqAllocator<std::uint8_t> alloc(frame);
  EXPECT_THROW(alloc.allocate(64), std::bad_alloc);
}

TEST(ZmqAllocator, EqualityComparesFrames)
{
  auto frame_a = make_frame_with({1, 2});
  auto frame_b = make_frame_with({3, 4});
  ZmqAllocator<std::uint8_t> a1(frame_a);
  ZmqAllocator<std::uint8_t> a2(frame_a);
  ZmqAllocator<std::uint8_t> b(frame_b);
  EXPECT_TRUE(a1 == a2);
  EXPECT_FALSE(a1 == b);
}

TEST(ZmqAllocator, KeepsFrameAliveAcrossVectorMove)
{
  auto frame = make_frame_with({9, 8, 7, 6});
  void * raw = frame->data();
  std::weak_ptr<zmq::message_t> weak = frame;

  std::vector<std::uint8_t, ZmqAllocator<std::uint8_t>> v1(
    4, ZmqAllocator<std::uint8_t>(frame));
  frame.reset();  // drop the local reference; only the allocator holds it now.

  auto v2 = std::move(v1);
  EXPECT_FALSE(weak.expired()) << "vector move must propagate the allocator";
  EXPECT_EQ(reinterpret_cast<void *>(v2.data()), raw);

  // Confirm the writable storage actually points into the frame: writing
  // through v2 should be observable at the original buffer address.
  v2[0] = 0xa5;
  EXPECT_EQ(static_cast<std::uint8_t *>(raw)[0], 0xa5);
}
