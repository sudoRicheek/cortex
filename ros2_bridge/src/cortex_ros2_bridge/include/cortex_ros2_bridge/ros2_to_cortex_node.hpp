// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#ifndef CORTEX_ROS2_BRIDGE__ROS2_TO_CORTEX_NODE_HPP_
#define CORTEX_ROS2_BRIDGE__ROS2_TO_CORTEX_NODE_HPP_

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

// Composable rclcpp::Node that bridges every `ros2_to_cortex` entry in its
// YAML config from a ROS 2 topic to a Cortex IPC publisher. One PUB socket
// per entry; the discovery daemon is told about each on startup and
// unregistered on shutdown.
//
// Parameters:
//   config_path  (string)  — path to the bridge YAML config (required).
class Ros2ToCortexBridge : public rclcpp::Node
{
public:
  explicit Ros2ToCortexBridge(const rclcpp::NodeOptions & options);
  ~Ros2ToCortexBridge() override;

  std::size_t num_active_bindings() const noexcept;

private:
  void initialize();
  std::string make_pub_endpoint(const std::string & entry_name) const;

  BridgeConfig config_;
  std::shared_ptr<zmq::context_t> ctx_;
  std::unique_ptr<cortex_wire::DiscoveryClient> discovery_;
  std::vector<std::unique_ptr<Ros2ToCortexBindingBase>> bindings_;
  // Topics we successfully registered with discovery, so we can unregister
  // them in the destructor.
  std::vector<std::string> registered_topics_;
};

}  // namespace cortex_ros2_bridge

#endif  // CORTEX_ROS2_BRIDGE__ROS2_TO_CORTEX_NODE_HPP_
