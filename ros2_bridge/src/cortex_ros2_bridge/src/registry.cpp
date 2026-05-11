// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#include "cortex_ros2_bridge/registry.hpp"

namespace cortex_ros2_bridge
{

bool AdapterRegistry::has_cortex_to_ros2(
  cortex_wire::MessageKind kind, std::string_view ros2_type_name) const
{
  std::lock_guard<std::mutex> g(mu_);
  return c2r_.find(Key{kind, std::string(ros2_type_name)}) != c2r_.end();
}

bool AdapterRegistry::has_ros2_to_cortex(
  cortex_wire::MessageKind kind, std::string_view ros2_type_name) const
{
  std::lock_guard<std::mutex> g(mu_);
  return r2c_.find(Key{kind, std::string(ros2_type_name)}) != r2c_.end();
}

AdapterRegistry & AdapterRegistry::global()
{
  // Meyers singleton; safe across multiple translation-unit initializers.
  static AdapterRegistry instance;
  return instance;
}

}  // namespace cortex_ros2_bridge
