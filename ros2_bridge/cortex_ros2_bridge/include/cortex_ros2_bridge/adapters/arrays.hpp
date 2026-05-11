// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#ifndef CORTEX_ROS2_BRIDGE__ADAPTERS__ARRAYS_HPP_
#define CORTEX_ROS2_BRIDGE__ADAPTERS__ARRAYS_HPP_

#include <std_msgs/msg/float32_multi_array.hpp>
#include <std_msgs/msg/float64_multi_array.hpp>

#include "cortex_ros2_bridge/adapter.hpp"
#include "cortex_ros2_bridge/registry.hpp"

namespace cortex_ros2_bridge::adapters
{

// ArrayMessage(data: ndarray, name: str = "", frame_id: str = "") <->
//   std_msgs/msg/Float32MultiArray | Float64MultiArray
//
// We provide one adapter per numeric dtype the user can plausibly want as a
// ROS 2 MultiArray. The YAML's ros2.type selects which one is wired up; if
// the wire dtype doesn't match the adapter's expected dtype, the adapter
// throws a decode error rather than silently misinterpreting bytes.

class ArrayFloat32Adapter
  : public BidirectionalAdapter<std_msgs::msg::Float32MultiArray>
{
public:
  cortex_wire::MessageKind cortex_kind() const override;
  std::string_view ros2_type_name() const override;
  std::unique_ptr<std_msgs::msg::Float32MultiArray> to_ros2(
    const CortexInbound & in) const override;
  CortexOutbound to_cortex(
    const std_msgs::msg::Float32MultiArray & msg, std::uint64_t sequence,
    const BridgeEntry & cfg) const override;
};

class ArrayFloat64Adapter
  : public BidirectionalAdapter<std_msgs::msg::Float64MultiArray>
{
public:
  cortex_wire::MessageKind cortex_kind() const override;
  std::string_view ros2_type_name() const override;
  std::unique_ptr<std_msgs::msg::Float64MultiArray> to_ros2(
    const CortexInbound & in) const override;
  CortexOutbound to_cortex(
    const std_msgs::msg::Float64MultiArray & msg, std::uint64_t sequence,
    const BridgeEntry & cfg) const override;
};

std::size_t register_array_adapters(AdapterRegistry & registry);

}  // namespace cortex_ros2_bridge::adapters

#endif  // CORTEX_ROS2_BRIDGE__ADAPTERS__ARRAYS_HPP_
