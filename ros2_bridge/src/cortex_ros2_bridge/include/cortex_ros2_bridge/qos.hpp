// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#ifndef CORTEX_ROS2_BRIDGE__QOS_HPP_
#define CORTEX_ROS2_BRIDGE__QOS_HPP_

#include <rclcpp/qos.hpp>

#include "cortex_ros2_bridge/config.hpp"

namespace cortex_ros2_bridge
{

// Translate a YAML-derived QosSettings into an rclcpp::QoS instance.
rclcpp::QoS make_qos(const QosSettings & q);

}  // namespace cortex_ros2_bridge

#endif  // CORTEX_ROS2_BRIDGE__QOS_HPP_
