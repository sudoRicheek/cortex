// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#ifndef CORTEX_ROS2_BRIDGE__REGISTRY_HPP_
#define CORTEX_ROS2_BRIDGE__REGISTRY_HPP_

#include <cortex_wire/fingerprint_table.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>

#include "cortex_ros2_bridge/adapter.hpp"

namespace cortex_ros2_bridge
{

// Type-erased registry of adapters keyed by (MessageKind, ros2_type_name).
//
// Design constraints / non-constraints:
//  - The registry stores `std::shared_ptr<void>` to erase the Ros2Msg
//    template parameter. Callers look up adapters with the concrete
//    template type at compile time; the cast is checked via the stored
//    `typeid` of the originally-registered Ros2Msg type.
//  - Lookups are O(N) over the small table (~16 entries); not worth a
//    hash map. The registry is read-mostly: writes happen once at
//    component construction, reads happen per message on hot paths.
//  - Adapters are owned via shared_ptr because the bridge components in
//    PR4/PR5 will each take a reference; one adapter instance per
//    process is fine for stateless primitives.
class AdapterRegistry
{
public:
  // Register a Cortex → ROS 2 adapter. Returns false if (kind, ros2_type)
  // is already registered for that direction.
  template<typename Ros2Msg>
  bool register_cortex_to_ros2(std::shared_ptr<CortexToRos2Adapter<Ros2Msg>> adapter);

  // Register a ROS 2 → Cortex adapter.
  template<typename Ros2Msg>
  bool register_ros2_to_cortex(std::shared_ptr<Ros2ToCortexAdapter<Ros2Msg>> adapter);

  // Convenience: register both directions of a bidirectional adapter at once.
  template<typename Ros2Msg>
  bool register_bidirectional(std::shared_ptr<BidirectionalAdapter<Ros2Msg>> adapter);

  // Look up a Cortex → ROS 2 adapter by (kind, ros2_type_name). Returns
  // nullptr if no adapter is registered. The template parameter must
  // match the type the adapter was registered with; a mismatch returns
  // nullptr (we use typeid to detect this).
  template<typename Ros2Msg>
  std::shared_ptr<CortexToRos2Adapter<Ros2Msg>> find_cortex_to_ros2(
    cortex_wire::MessageKind kind, std::string_view ros2_type_name) const;

  template<typename Ros2Msg>
  std::shared_ptr<Ros2ToCortexAdapter<Ros2Msg>> find_ros2_to_cortex(
    cortex_wire::MessageKind kind, std::string_view ros2_type_name) const;

  // Returns true if any adapter exists for (kind, ros2_type_name) in the
  // given direction. Used by config validation to fail fast before the
  // component starts wiring sockets.
  bool has_cortex_to_ros2(cortex_wire::MessageKind kind, std::string_view ros2_type_name) const;
  bool has_ros2_to_cortex(cortex_wire::MessageKind kind, std::string_view ros2_type_name) const;

  // Process-wide registry. Adapter modules register into this at static
  // initialization via the REGISTER_* helpers below. Tests can use a
  // fresh AdapterRegistry instead.
  static AdapterRegistry & global();

private:
  struct Entry
  {
    std::shared_ptr<void> adapter;   // CortexToRos2Adapter<T> / Ros2ToCortexAdapter<T>
    std::type_index ros2_msg_type;
    std::string ros2_type_name;
  };
  using Key = std::pair<cortex_wire::MessageKind, std::string>;
  struct KeyHash
  {
    std::size_t operator()(const Key & k) const noexcept
    {
      return std::hash<std::string>{}(k.second) ^
             (static_cast<std::size_t>(k.first) * 0x9e3779b97f4a7c15ULL);
    }
  };

  mutable std::mutex mu_;
  std::unordered_map<Key, Entry, KeyHash> c2r_;
  std::unordered_map<Key, Entry, KeyHash> r2c_;
};

// ---- template definitions ------------------------------------------------

template<typename Ros2Msg>
bool AdapterRegistry::register_cortex_to_ros2(
  std::shared_ptr<CortexToRos2Adapter<Ros2Msg>> adapter)
{
  if (!adapter) {return false;}
  const cortex_wire::MessageKind kind = adapter->cortex_kind();
  const std::string type_name(adapter->ros2_type_name());
  std::lock_guard<std::mutex> g(mu_);
  Entry e{
    std::static_pointer_cast<void>(adapter),
    std::type_index(typeid(Ros2Msg)),
    type_name,
  };
  return c2r_.emplace(Key{kind, type_name}, std::move(e)).second;
}

template<typename Ros2Msg>
bool AdapterRegistry::register_ros2_to_cortex(
  std::shared_ptr<Ros2ToCortexAdapter<Ros2Msg>> adapter)
{
  if (!adapter) {return false;}
  const cortex_wire::MessageKind kind = adapter->cortex_kind();
  const std::string type_name(adapter->ros2_type_name());
  std::lock_guard<std::mutex> g(mu_);
  Entry e{
    std::static_pointer_cast<void>(adapter),
    std::type_index(typeid(Ros2Msg)),
    type_name,
  };
  return r2c_.emplace(Key{kind, type_name}, std::move(e)).second;
}

template<typename Ros2Msg>
bool AdapterRegistry::register_bidirectional(
  std::shared_ptr<BidirectionalAdapter<Ros2Msg>> adapter)
{
  std::shared_ptr<CortexToRos2Adapter<Ros2Msg>> c2r = adapter;
  std::shared_ptr<Ros2ToCortexAdapter<Ros2Msg>> r2c = adapter;
  const bool a = register_cortex_to_ros2<Ros2Msg>(std::move(c2r));
  const bool b = register_ros2_to_cortex<Ros2Msg>(std::move(r2c));
  return a && b;
}

template<typename Ros2Msg>
std::shared_ptr<CortexToRos2Adapter<Ros2Msg>> AdapterRegistry::find_cortex_to_ros2(
  cortex_wire::MessageKind kind, std::string_view ros2_type_name) const
{
  std::lock_guard<std::mutex> g(mu_);
  auto it = c2r_.find(Key{kind, std::string(ros2_type_name)});
  if (it == c2r_.end()) {return nullptr;}
  if (it->second.ros2_msg_type != std::type_index(typeid(Ros2Msg))) {
    return nullptr;
  }
  return std::static_pointer_cast<CortexToRos2Adapter<Ros2Msg>>(it->second.adapter);
}

template<typename Ros2Msg>
std::shared_ptr<Ros2ToCortexAdapter<Ros2Msg>> AdapterRegistry::find_ros2_to_cortex(
  cortex_wire::MessageKind kind, std::string_view ros2_type_name) const
{
  std::lock_guard<std::mutex> g(mu_);
  auto it = r2c_.find(Key{kind, std::string(ros2_type_name)});
  if (it == r2c_.end()) {return nullptr;}
  if (it->second.ros2_msg_type != std::type_index(typeid(Ros2Msg))) {
    return nullptr;
  }
  return std::static_pointer_cast<Ros2ToCortexAdapter<Ros2Msg>>(it->second.adapter);
}

}  // namespace cortex_ros2_bridge

#endif  // CORTEX_ROS2_BRIDGE__REGISTRY_HPP_
