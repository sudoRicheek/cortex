// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#ifndef CORTEX_ROS2_BRIDGE__ADAPTERS__PRIMITIVES_HPP_
#define CORTEX_ROS2_BRIDGE__ADAPTERS__PRIMITIVES_HPP_

#include <builtin_interfaces/msg/time.hpp>
#include <std_msgs/msg/byte_multi_array.hpp>
#include <std_msgs/msg/float64.hpp>
#include <std_msgs/msg/header.hpp>
#include <std_msgs/msg/int64.hpp>
#include <std_msgs/msg/string.hpp>

#include "cortex_ros2_bridge/adapter.hpp"
#include "cortex_ros2_bridge/registry.hpp"

namespace cortex_ros2_bridge::adapters
{

class StringAdapter : public BidirectionalAdapter<std_msgs::msg::String>
{
public:
  cortex_wire::MessageKind cortex_kind() const override;
  std::string_view ros2_type_name() const override;
  std::unique_ptr<std_msgs::msg::String> to_ros2(const CortexInbound & in) const override;
  CortexOutbound to_cortex(
    const std_msgs::msg::String & msg, std::uint64_t sequence,
    const BridgeEntry & cfg) const override;
};

class IntAdapter : public BidirectionalAdapter<std_msgs::msg::Int64>
{
public:
  cortex_wire::MessageKind cortex_kind() const override;
  std::string_view ros2_type_name() const override;
  std::unique_ptr<std_msgs::msg::Int64> to_ros2(const CortexInbound & in) const override;
  CortexOutbound to_cortex(
    const std_msgs::msg::Int64 & msg, std::uint64_t sequence,
    const BridgeEntry & cfg) const override;
};

class FloatAdapter : public BidirectionalAdapter<std_msgs::msg::Float64>
{
public:
  cortex_wire::MessageKind cortex_kind() const override;
  std::string_view ros2_type_name() const override;
  std::unique_ptr<std_msgs::msg::Float64> to_ros2(const CortexInbound & in) const override;
  CortexOutbound to_cortex(
    const std_msgs::msg::Float64 & msg, std::uint64_t sequence,
    const BridgeEntry & cfg) const override;
};

class BytesAdapter : public BidirectionalAdapter<std_msgs::msg::ByteMultiArray>
{
public:
  cortex_wire::MessageKind cortex_kind() const override;
  std::string_view ros2_type_name() const override;
  std::unique_ptr<std_msgs::msg::ByteMultiArray> to_ros2(const CortexInbound & in) const override;
  CortexOutbound to_cortex(
    const std_msgs::msg::ByteMultiArray & msg, std::uint64_t sequence,
    const BridgeEntry & cfg) const override;
};

class TimestampAdapter : public BidirectionalAdapter<builtin_interfaces::msg::Time>
{
public:
  cortex_wire::MessageKind cortex_kind() const override;
  std::string_view ros2_type_name() const override;
  std::unique_ptr<builtin_interfaces::msg::Time> to_ros2(const CortexInbound & in) const override;
  CortexOutbound to_cortex(
    const builtin_interfaces::msg::Time & msg, std::uint64_t sequence,
    const BridgeEntry & cfg) const override;
};

class HeaderAdapter : public BidirectionalAdapter<std_msgs::msg::Header>
{
public:
  cortex_wire::MessageKind cortex_kind() const override;
  std::string_view ros2_type_name() const override;
  std::unique_ptr<std_msgs::msg::Header> to_ros2(const CortexInbound & in) const override;
  CortexOutbound to_cortex(
    const std_msgs::msg::Header & msg, std::uint64_t sequence,
    const BridgeEntry & cfg) const override;
};

// Register all primitive adapters into the given registry. Returns the
// number of newly-registered entries (each direction counts separately).
// Idempotent — re-registration returns 0.
std::size_t register_primitives(AdapterRegistry & registry);

}  // namespace cortex_ros2_bridge::adapters

#endif  // CORTEX_ROS2_BRIDGE__ADAPTERS__PRIMITIVES_HPP_
