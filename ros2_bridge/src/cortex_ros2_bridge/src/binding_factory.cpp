// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#include "cortex_ros2_bridge/binding_factory.hpp"

#include <builtin_interfaces/msg/time.hpp>
#include <geometry_msgs/msg/pose_stamped.hpp>
#include <geometry_msgs/msg/transform_stamped.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <sensor_msgs/msg/point_cloud2.hpp>
#include <std_msgs/msg/byte_multi_array.hpp>
#include <std_msgs/msg/float32_multi_array.hpp>
#include <std_msgs/msg/float64.hpp>
#include <std_msgs/msg/float64_multi_array.hpp>
#include <std_msgs/msg/header.hpp>
#include <std_msgs/msg/int64.hpp>
#include <std_msgs/msg/string.hpp>

namespace cortex_ros2_bridge
{

bool BindingFactoryRegistry::register_cortex_to_ros2(
  std::string ros2_type_name, CortexToRos2BindingFactory factory)
{
  if (!factory) {return false;}
  std::lock_guard<std::mutex> g(mu_);
  return c2r_.emplace(std::move(ros2_type_name), std::move(factory)).second;
}

bool BindingFactoryRegistry::register_ros2_to_cortex(
  std::string ros2_type_name, Ros2ToCortexBindingFactory factory)
{
  if (!factory) {return false;}
  std::lock_guard<std::mutex> g(mu_);
  return r2c_.emplace(std::move(ros2_type_name), std::move(factory)).second;
}

CortexToRos2BindingFactory BindingFactoryRegistry::get_cortex_to_ros2(
  std::string_view ros2_type_name) const
{
  std::lock_guard<std::mutex> g(mu_);
  const auto it = c2r_.find(std::string(ros2_type_name));
  if (it == c2r_.end()) {return {};}
  return it->second;
}

Ros2ToCortexBindingFactory BindingFactoryRegistry::get_ros2_to_cortex(
  std::string_view ros2_type_name) const
{
  std::lock_guard<std::mutex> g(mu_);
  const auto it = r2c_.find(std::string(ros2_type_name));
  if (it == r2c_.end()) {return {};}
  return it->second;
}

BindingFactoryRegistry & BindingFactoryRegistry::global()
{
  static BindingFactoryRegistry instance;
  return instance;
}

std::size_t register_primitive_bindings(BindingFactoryRegistry & reg)
{
  std::size_t added = 0;
  added += register_cortex_to_ros2_binding<std_msgs::msg::String>(
    reg, "std_msgs/msg/String") ? 1 : 0;
  added += register_cortex_to_ros2_binding<std_msgs::msg::Int64>(
    reg, "std_msgs/msg/Int64") ? 1 : 0;
  added += register_cortex_to_ros2_binding<std_msgs::msg::Float64>(
    reg, "std_msgs/msg/Float64") ? 1 : 0;
  added += register_cortex_to_ros2_binding<std_msgs::msg::ByteMultiArray>(
    reg, "std_msgs/msg/ByteMultiArray") ? 1 : 0;
  added += register_cortex_to_ros2_binding<builtin_interfaces::msg::Time>(
    reg, "builtin_interfaces/msg/Time") ? 1 : 0;
  added += register_cortex_to_ros2_binding<std_msgs::msg::Header>(
    reg, "std_msgs/msg/Header") ? 1 : 0;

  added += register_ros2_to_cortex_binding<std_msgs::msg::String>(
    reg, "std_msgs/msg/String") ? 1 : 0;
  added += register_ros2_to_cortex_binding<std_msgs::msg::Int64>(
    reg, "std_msgs/msg/Int64") ? 1 : 0;
  added += register_ros2_to_cortex_binding<std_msgs::msg::Float64>(
    reg, "std_msgs/msg/Float64") ? 1 : 0;
  added += register_ros2_to_cortex_binding<std_msgs::msg::ByteMultiArray>(
    reg, "std_msgs/msg/ByteMultiArray") ? 1 : 0;
  added += register_ros2_to_cortex_binding<builtin_interfaces::msg::Time>(
    reg, "builtin_interfaces/msg/Time") ? 1 : 0;
  added += register_ros2_to_cortex_binding<std_msgs::msg::Header>(
    reg, "std_msgs/msg/Header") ? 1 : 0;
  return added;
}

std::size_t register_standard_bindings(BindingFactoryRegistry & reg)
{
  std::size_t added = 0;
  added += register_cortex_to_ros2_binding<std_msgs::msg::Float32MultiArray>(
    reg, "std_msgs/msg/Float32MultiArray") ? 1 : 0;
  added += register_cortex_to_ros2_binding<std_msgs::msg::Float64MultiArray>(
    reg, "std_msgs/msg/Float64MultiArray") ? 1 : 0;
  added += register_cortex_to_ros2_binding<sensor_msgs::msg::Image>(
    reg, "sensor_msgs/msg/Image") ? 1 : 0;
  added += register_cortex_to_ros2_binding<sensor_msgs::msg::PointCloud2>(
    reg, "sensor_msgs/msg/PointCloud2") ? 1 : 0;
  added += register_cortex_to_ros2_binding<geometry_msgs::msg::PoseStamped>(
    reg, "geometry_msgs/msg/PoseStamped") ? 1 : 0;
  added += register_cortex_to_ros2_binding<geometry_msgs::msg::TransformStamped>(
    reg, "geometry_msgs/msg/TransformStamped") ? 1 : 0;

  added += register_ros2_to_cortex_binding<std_msgs::msg::Float32MultiArray>(
    reg, "std_msgs/msg/Float32MultiArray") ? 1 : 0;
  added += register_ros2_to_cortex_binding<std_msgs::msg::Float64MultiArray>(
    reg, "std_msgs/msg/Float64MultiArray") ? 1 : 0;
  added += register_ros2_to_cortex_binding<sensor_msgs::msg::Image>(
    reg, "sensor_msgs/msg/Image") ? 1 : 0;
  added += register_ros2_to_cortex_binding<sensor_msgs::msg::PointCloud2>(
    reg, "sensor_msgs/msg/PointCloud2") ? 1 : 0;
  added += register_ros2_to_cortex_binding<geometry_msgs::msg::PoseStamped>(
    reg, "geometry_msgs/msg/PoseStamped") ? 1 : 0;
  added += register_ros2_to_cortex_binding<geometry_msgs::msg::TransformStamped>(
    reg, "geometry_msgs/msg/TransformStamped") ? 1 : 0;
  return added;
}

}  // namespace cortex_ros2_bridge
