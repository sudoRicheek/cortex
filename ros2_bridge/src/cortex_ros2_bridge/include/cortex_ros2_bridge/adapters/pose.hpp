// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#ifndef CORTEX_ROS2_BRIDGE__ADAPTERS__POSE_HPP_
#define CORTEX_ROS2_BRIDGE__ADAPTERS__POSE_HPP_

#include <geometry_msgs/msg/pose_stamped.hpp>

#include "cortex_ros2_bridge/adapter.hpp"
#include "cortex_ros2_bridge/registry.hpp"

namespace cortex_ros2_bridge::adapters
{

// PoseMessage(position, orientation, frame_id, child_frame_id) <->
//   geometry_msgs/msg/PoseStamped
//
// Forward: position is [x,y,z] (float64), orientation is [qx,qy,qz,qw]
// (float64). child_frame_id is dropped (PoseStamped has no such field).
class PoseAdapter : public BidirectionalAdapter<geometry_msgs::msg::PoseStamped>
{
public:
  cortex_wire::MessageKind cortex_kind() const override;
  std::string_view ros2_type_name() const override;
  std::unique_ptr<geometry_msgs::msg::PoseStamped> to_ros2(
    const CortexInbound & in) const override;
  CortexOutbound to_cortex(
    const geometry_msgs::msg::PoseStamped & msg, std::uint64_t sequence,
    const BridgeEntry & cfg) const override;
};

std::size_t register_pose_adapters(AdapterRegistry & registry);

}  // namespace cortex_ros2_bridge::adapters

#endif  // CORTEX_ROS2_BRIDGE__ADAPTERS__POSE_HPP_
