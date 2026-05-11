// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#ifndef CORTEX_ROS2_BRIDGE__ADAPTERS__TRANSFORM_HPP_
#define CORTEX_ROS2_BRIDGE__ADAPTERS__TRANSFORM_HPP_

#include <geometry_msgs/msg/transform_stamped.hpp>

#include "cortex_ros2_bridge/adapter.hpp"
#include "cortex_ros2_bridge/registry.hpp"

namespace cortex_ros2_bridge::adapters
{

// TransformMessage(matrix: 4x4 float, frame_id, child_frame_id) <->
//   geometry_msgs/msg/TransformStamped
//
// Forward: decomposes the 4x4 into translation (matrix[0:3, 3]) and the
// quaternion of matrix[0:3, 0:3] via Shepperd's method. Reverse: builds the
// 4x4 from the ROS translation+rotation.
//
// `/tf` broadcasting is deliberately out of scope for v1 — the YAML's
// broadcast_tf hook is unwired here.
class TransformAdapter
  : public BidirectionalAdapter<geometry_msgs::msg::TransformStamped>
{
public:
  cortex_wire::MessageKind cortex_kind() const override;
  std::string_view ros2_type_name() const override;
  std::unique_ptr<geometry_msgs::msg::TransformStamped> to_ros2(
    const CortexInbound & in) const override;
  CortexOutbound to_cortex(
    const geometry_msgs::msg::TransformStamped & msg, std::uint64_t sequence,
    const BridgeEntry & cfg) const override;
};

std::size_t register_transform_adapters(AdapterRegistry & registry);

}  // namespace cortex_ros2_bridge::adapters

#endif  // CORTEX_ROS2_BRIDGE__ADAPTERS__TRANSFORM_HPP_
