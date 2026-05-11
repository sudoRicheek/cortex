// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#include "cortex_ros2_bridge/qos.hpp"

namespace cortex_ros2_bridge
{

rclcpp::QoS make_qos(const QosSettings & q)
{
  // History first — the depth ctor variants of rclcpp::QoS encode "keep last
  // with depth N"; KeepAll is built via KeepAll() (which sets the underlying
  // RMW history to RMW_QOS_POLICY_HISTORY_KEEP_ALL and ignores depth).
  rclcpp::QoS qos = (q.history == History::KeepAll)
    ? rclcpp::QoS(rclcpp::KeepAll())
    : rclcpp::QoS(rclcpp::KeepLast(q.depth));

  if (q.reliability == Reliability::BestEffort) {
    qos.best_effort();
  } else {
    qos.reliable();
  }

  if (q.durability == Durability::TransientLocal) {
    qos.transient_local();
  } else {
    qos.durability_volatile();
  }
  return qos;
}

}  // namespace cortex_ros2_bridge
