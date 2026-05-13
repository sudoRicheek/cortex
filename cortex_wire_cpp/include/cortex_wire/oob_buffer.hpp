// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#ifndef CORTEX_WIRE__OOB_BUFFER_HPP_
#define CORTEX_WIRE__OOB_BUFFER_HPP_

#include <zmq.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace cortex_wire
{

// Shared, immutable view into a ZMQ multipart frame. The shared_ptr owns the
// zmq::message_t so the memory stays alive as long as any OobBuffer or
// ZmqAllocator referencing it does.
using ZmqFramePtr = std::shared_ptr<zmq::message_t>;

inline ZmqFramePtr make_owned(zmq::message_t && msg)
{
  return std::make_shared<zmq::message_t>(std::move(msg));
}

// Vector-like view over a slice of a ZMQ frame. Read-only access; iterators
// and data() return the frame's memory directly — no copy.
//
// Use this when you need a contiguous, typed view that you can hand to
// `std::span`, range-based for, or any code that takes data()+size(). Not a
// std::vector — for stdlib-vector compatibility see ZmqAllocator below.
template <typename T>
class OobBuffer
{
public:
  static_assert(std::is_trivially_copyable_v<T>, "OobBuffer<T> requires trivially-copyable T");

  using value_type = T;
  using size_type = std::size_t;
  using const_iterator = const T *;

  OobBuffer() = default;

  OobBuffer(ZmqFramePtr frame, size_type element_count, size_type byte_offset = 0)
  : frame_(std::move(frame)), offset_(byte_offset), count_(element_count)
  {
  }

  const T * data() const noexcept
  {
    if (!frame_) {
      return nullptr;
    }
    return reinterpret_cast<const T *>(
      static_cast<const std::uint8_t *>(frame_->data()) + offset_);
  }

  size_type size() const noexcept {return count_;}
  size_type size_bytes() const noexcept {return count_ * sizeof(T);}
  bool empty() const noexcept {return count_ == 0;}

  const_iterator begin() const noexcept {return data();}
  const_iterator end() const noexcept {return data() + count_;}

  const T & operator[](size_type i) const noexcept {return data()[i];}

  // Returns the underlying owned frame; useful for handing the same lifetime
  // into a custom allocator or another OobBuffer slice.
  const ZmqFramePtr & frame() const noexcept {return frame_;}

private:
  ZmqFramePtr frame_;
  size_type offset_ = 0;
  size_type count_ = 0;
};

// Stateful allocator backed by an existing ZMQ frame buffer. Intended for
// the narrow case where downstream code requires a std::vector<T> and we
// want the vector's storage to coincide with a ZMQ frame so the frame's
// lifetime is extended for free while the vector is alive.
//
// Important caveats — read before using:
//  - allocate(n) returns the frame's existing buffer (offset-adjusted) and
//    throws std::bad_alloc if the request exceeds the frame size. It is a
//    *non-allocating* allocator; reserve/push_back/resize past the initial
//    capacity will throw.
//  - std::vector value-initialises elements in its sizing constructors and
//    in `resize`, so constructing a vector this way **will overwrite the
//    frame contents** with T(). The allocator is useful for the symmetric
//    case where the vector is *populated* (memcpy or assignment) and you
//    just want the underlying allocation to be the ZMQ frame — saves one
//    allocation per message and ties lifetimes together.
//  - If you need a read-only view of the frame *without* overwriting its
//    bytes, use OobBuffer<T> above instead.
//  - Two allocators compare equal iff they reference the same frame at the
//    same offset.
template <typename T>
class ZmqAllocator
{
public:
  using value_type = T;
  using propagate_on_container_copy_assignment = std::true_type;
  using propagate_on_container_move_assignment = std::true_type;
  using propagate_on_container_swap = std::true_type;
  using is_always_equal = std::false_type;

  ZmqAllocator() = default;

  explicit ZmqAllocator(ZmqFramePtr frame, std::size_t byte_offset = 0) noexcept
  : frame_(std::move(frame)), offset_(byte_offset)
  {
  }

  template <typename U>
  ZmqAllocator(const ZmqAllocator<U> & other) noexcept
  : frame_(other.frame_), offset_(other.offset_)
  {
  }

  T * allocate(std::size_t n)
  {
    if (!frame_) {
      throw std::bad_alloc();
    }
    if (n * sizeof(T) > frame_->size() - offset_) {
      throw std::bad_alloc();
    }
    return reinterpret_cast<T *>(
      static_cast<std::uint8_t *>(frame_->data()) + offset_);
  }

  void deallocate(T *, std::size_t) noexcept
  {
    // No-op; the shared_ptr deallocates when the last ZmqAllocator copy dies.
  }

  template <typename U>
  bool operator==(const ZmqAllocator<U> & other) const noexcept
  {
    return frame_ == other.frame_ && offset_ == other.offset_;
  }
  template <typename U>
  bool operator!=(const ZmqAllocator<U> & other) const noexcept
  {
    return !(*this == other);
  }

  const ZmqFramePtr & frame() const noexcept {return frame_;}

  template <typename U>
  friend class ZmqAllocator;

private:
  ZmqFramePtr frame_;
  std::size_t offset_ = 0;
};

}  // namespace cortex_wire

#endif  // CORTEX_WIRE__OOB_BUFFER_HPP_
