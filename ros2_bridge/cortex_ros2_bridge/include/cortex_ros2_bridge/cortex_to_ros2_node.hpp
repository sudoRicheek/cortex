// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#ifndef CORTEX_ROS2_BRIDGE__CORTEX_TO_ROS2_NODE_HPP_
#define CORTEX_ROS2_BRIDGE__CORTEX_TO_ROS2_NODE_HPP_

#include <cortex_wire/discovery_client.hpp>
#include <rclcpp/rclcpp.hpp>
#include <zmq.hpp>

#include <memory>
#include <vector>

#include "cortex_ros2_bridge/binding.hpp"
#include "cortex_ros2_bridge/binding_factory.hpp"
#include "cortex_ros2_bridge/config.hpp"
#include "cortex_ros2_bridge/registry.hpp"

namespace cortex_ros2_bridge
{

// Composable rclcpp::Node that bridges every `cortex_to_ros2` entry in its
// YAML config from Cortex IPC to a ROS 2 topic. One SUB socket and one
// recv thread per entry.
//
// Parameters:
//   config_path  (string)  — path to the bridge YAML config (required).
//
// Adapters and binding factories are looked up in the process-global
// registries. The primitive set is auto-registered on first instantiation.
class CortexToRos2Bridge : public rclcpp::Node
{
public:
  explicit CortexToRos2Bridge(const rclcpp::NodeOptions & options);
  ~CortexToRos2Bridge() override;

  // Visible for tests: number of bridge entries successfully wired up.
  std::size_t num_active_bindings() const noexcept;

private:
  void initialize();

  BridgeConfig config_;
  std::shared_ptr<zmq::context_t> ctx_;
  std::unique_ptr<cortex_wire::DiscoveryClient> discovery_;
  std::vector<std::unique_ptr<CortexToRos2BindingBase>> bindings_;
};

}  // namespace cortex_ros2_bridge

#endif  // CORTEX_ROS2_BRIDGE__CORTEX_TO_ROS2_NODE_HPP_
