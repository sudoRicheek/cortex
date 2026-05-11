// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#ifndef CORTEX_ROS2_BRIDGE__BINDING_FACTORY_HPP_
#define CORTEX_ROS2_BRIDGE__BINDING_FACTORY_HPP_

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "cortex_ros2_bridge/binding.hpp"
#include "cortex_ros2_bridge/registry.hpp"

namespace cortex_ros2_bridge
{

// Factories produce a typed binding from a string-keyed dispatch on the ROS 2
// type name. The component looks up the factory once per YAML entry; the
// returned binding hides the templated rclcpp::Publisher / Subscription
// behind the non-templated BindingBase interface.

using CortexToRos2BindingFactory = std::function<
  std::unique_ptr<CortexToRos2BindingBase>(
    rclcpp::Node *, zmq::context_t *,
    const BridgeEntry &, const cortex_wire::TopicInfo &,
    cortex_wire::MessageKind, const AdapterRegistry &,
    const rclcpp::QoS &)>;

using Ros2ToCortexBindingFactory = std::function<
  std::unique_ptr<Ros2ToCortexBindingBase>(
    rclcpp::Node *, zmq::context_t *,
    const BridgeEntry &, cortex_wire::MessageKind,
    std::uint64_t /*fingerprint*/, std::string /*pub_endpoint*/,
    const AdapterRegistry &, const rclcpp::QoS &)>;

class BindingFactoryRegistry
{
public:
  bool register_cortex_to_ros2(std::string ros2_type_name, CortexToRos2BindingFactory factory);
  bool register_ros2_to_cortex(std::string ros2_type_name, Ros2ToCortexBindingFactory factory);

  // Returns an empty std::function if not registered.
  CortexToRos2BindingFactory get_cortex_to_ros2(std::string_view ros2_type_name) const;
  Ros2ToCortexBindingFactory get_ros2_to_cortex(std::string_view ros2_type_name) const;

  static BindingFactoryRegistry & global();

private:
  mutable std::mutex mu_;
  std::unordered_map<std::string, CortexToRos2BindingFactory> c2r_;
  std::unordered_map<std::string, Ros2ToCortexBindingFactory> r2c_;
};

// ---- helpers ---------------------------------------------------------------

template<typename Ros2Msg>
inline bool register_cortex_to_ros2_binding(
  BindingFactoryRegistry & reg, std::string ros2_type_name)
{
  return reg.register_cortex_to_ros2(
    ros2_type_name,
    [](rclcpp::Node * node, zmq::context_t * ctx,
    const BridgeEntry & cfg, const cortex_wire::TopicInfo & info,
    cortex_wire::MessageKind kind, const AdapterRegistry & adapters,
    const rclcpp::QoS & qos) -> std::unique_ptr<CortexToRos2BindingBase> {
      const auto ros2_type = cfg.ros2.type.value_or(std::string{});
      auto adapter = adapters.find_cortex_to_ros2<Ros2Msg>(kind, ros2_type);
      if (!adapter) {return nullptr;}
      return std::make_unique<CortexToRos2BindingImpl<Ros2Msg>>(
        node, ctx, cfg, info, std::move(adapter), qos);
    });
}

template<typename Ros2Msg>
inline bool register_ros2_to_cortex_binding(
  BindingFactoryRegistry & reg, std::string ros2_type_name)
{
  return reg.register_ros2_to_cortex(
    ros2_type_name,
    [](rclcpp::Node * node, zmq::context_t * ctx,
    const BridgeEntry & cfg, cortex_wire::MessageKind kind,
    std::uint64_t fingerprint, std::string pub_endpoint,
    const AdapterRegistry & adapters, const rclcpp::QoS & qos)
    -> std::unique_ptr<Ros2ToCortexBindingBase> {
      const auto ros2_type = cfg.ros2.type.value_or(std::string{});
      auto adapter = adapters.find_ros2_to_cortex<Ros2Msg>(kind, ros2_type);
      if (!adapter) {return nullptr;}
      return std::make_unique<Ros2ToCortexBindingImpl<Ros2Msg>>(
        node, ctx, cfg, std::move(adapter),
        std::move(pub_endpoint), fingerprint, qos);
    });
}

// Registers binding factories for the primitive catalogue. Pairs with
// adapters::register_primitives() — together they wire up every primitive
// in both directions.
std::size_t register_primitive_bindings(BindingFactoryRegistry & reg);

}  // namespace cortex_ros2_bridge

#endif  // CORTEX_ROS2_BRIDGE__BINDING_FACTORY_HPP_
