// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#ifndef CORTEX_ROS2_BRIDGE__ADAPTER_HPP_
#define CORTEX_ROS2_BRIDGE__ADAPTER_HPP_

#include <cortex_wire/fingerprint_table.hpp>
#include <cortex_wire/header.hpp>
#include <cortex_wire/metadata.hpp>
#include <cortex_wire/oob_buffer.hpp>

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "cortex_ros2_bridge/config.hpp"

namespace cortex_ros2_bridge
{

// Bundle of inputs handed to a Cortex → ROS 2 adapter call.
//
// Lifetime contract: `metadata` and `oob_frames` reference memory that the
// caller (the bridge component's recv loop) keeps alive at least until the
// adapter returns. Adapters that want to hold on to an OOB frame beyond
// the call must store a copy of the `ZmqFramePtr` (cheap — it's a
// shared_ptr); the bytes are never copied.
struct CortexInbound
{
  cortex_wire::MessageHeader header;
  const cortex_wire::DecodedMetadata & metadata;
  const std::vector<cortex_wire::ZmqFramePtr> & oob_frames;
  const BridgeEntry & cfg;
};

// Output of a ROS 2 → Cortex adapter call: a metadata frame plus the ordered
// list of OOB buffer frames to emit on the wire after it.
struct CortexOutbound
{
  std::vector<std::uint8_t> metadata;
  std::vector<std::vector<std::uint8_t>> oob_buffers;
};

// Adapter ABCs are templated on the ROS 2 message type so the bridge
// components can hold concrete `rclcpp::Publisher<Ros2Msg>` /
// `rclcpp::Subscription<Ros2Msg>` handles without runtime type erasure.
// Per-direction split lets adapters opt into one direction only.

template<typename Ros2Msg>
class CortexToRos2Adapter
{
public:
  virtual ~CortexToRos2Adapter() = default;

  // The Cortex message kind this adapter consumes. Used for fingerprint
  // verification at subscriber setup.
  virtual cortex_wire::MessageKind cortex_kind() const = 0;

  // The ROS 2 type string, e.g. "std_msgs/msg/String". Used as the registry
  // key alongside cortex_kind() so multiple adapters can coexist for the
  // same Cortex kind, mapping to different ROS 2 types.
  virtual std::string_view ros2_type_name() const = 0;

  // Build a ROS 2 message from a decoded Cortex inbound. The returned
  // unique_ptr is what `rclcpp::Publisher::publish` requires on the
  // intra-process zero-copy path.
  virtual std::unique_ptr<Ros2Msg> to_ros2(const CortexInbound & in) const = 0;
};

template<typename Ros2Msg>
class Ros2ToCortexAdapter
{
public:
  virtual ~Ros2ToCortexAdapter() = default;

  virtual cortex_wire::MessageKind cortex_kind() const = 0;
  virtual std::string_view ros2_type_name() const = 0;

  // Build the Cortex outbound frames from a ROS 2 message. `sequence` is the
  // per-publisher monotonic counter the component injects into the wire
  // header (the adapter does not own it).
  virtual CortexOutbound to_cortex(
    const Ros2Msg & msg, std::uint64_t sequence, const BridgeEntry & cfg) const = 0;
};

// A bidirectional adapter is just one that inherits both interfaces.
// Provided as a convenience for the common case (most primitive adapters).
template<typename Ros2Msg>
class BidirectionalAdapter
  : public CortexToRos2Adapter<Ros2Msg>, public Ros2ToCortexAdapter<Ros2Msg>
{
};

}  // namespace cortex_ros2_bridge

#endif  // CORTEX_ROS2_BRIDGE__ADAPTER_HPP_
