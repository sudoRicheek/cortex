// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#include "cortex_ros2_bridge/binding_factory.hpp"

#include <gtest/gtest.h>

using cortex_ros2_bridge::BindingFactoryRegistry;
using cortex_ros2_bridge::register_primitive_bindings;

TEST(BindingFactoryRegistry, RegistersAllTwelveDirections)
{
  BindingFactoryRegistry reg;
  EXPECT_EQ(register_primitive_bindings(reg), 12u);

  const std::vector<std::string> primitive_types = {
    "std_msgs/msg/String",
    "std_msgs/msg/Int64",
    "std_msgs/msg/Float64",
    "std_msgs/msg/ByteMultiArray",
    "builtin_interfaces/msg/Time",
    "std_msgs/msg/Header",
  };
  for (const auto & t : primitive_types) {
    EXPECT_TRUE(static_cast<bool>(reg.get_cortex_to_ros2(t))) << t;
    EXPECT_TRUE(static_cast<bool>(reg.get_ros2_to_cortex(t))) << t;
  }
}

TEST(BindingFactoryRegistry, IsIdempotent)
{
  BindingFactoryRegistry reg;
  register_primitive_bindings(reg);
  EXPECT_EQ(register_primitive_bindings(reg), 0u);
}

TEST(BindingFactoryRegistry, UnknownTypeReturnsEmpty)
{
  BindingFactoryRegistry reg;
  register_primitive_bindings(reg);
  EXPECT_FALSE(static_cast<bool>(reg.get_cortex_to_ros2("sensor_msgs/msg/NotAType")));
  EXPECT_FALSE(static_cast<bool>(reg.get_ros2_to_cortex("sensor_msgs/msg/NotAType")));
}

TEST(BindingFactoryRegistry, GlobalIsSingleton)
{
  EXPECT_EQ(&BindingFactoryRegistry::global(), &BindingFactoryRegistry::global());
}
