// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#ifndef CORTEX_ROS2_BRIDGE__ADAPTERS__TENSOR_HPP_
#define CORTEX_ROS2_BRIDGE__ADAPTERS__TENSOR_HPP_

#include <std_msgs/msg/float32_multi_array.hpp>

#include "cortex_ros2_bridge/adapter.hpp"
#include "cortex_ros2_bridge/registry.hpp"

namespace cortex_ros2_bridge::adapters
{

// TensorMessage(data: torch.Tensor, name: str = "") <->
//   std_msgs/msg/Float32MultiArray
//
// Forward: drops the torch-specific device / requires_grad metadata. The
// receiver (ROS side) gets a CPU array. Reverse: emits a torch OOB
// descriptor with device="cpu", requires_grad=false (the Python receiver
// reconstructs a CPU tensor).
class TensorFloat32Adapter
  : public BidirectionalAdapter<std_msgs::msg::Float32MultiArray>
{
public:
  cortex_wire::MessageKind cortex_kind() const override;
  std::string_view ros2_type_name() const override;
  std::unique_ptr<std_msgs::msg::Float32MultiArray> to_ros2(
    const CortexInbound & in) const override;
  CortexOutbound to_cortex(
    const std_msgs::msg::Float32MultiArray & msg, std::uint64_t sequence,
    const BridgeEntry & cfg) const override;
};

std::size_t register_tensor_adapters(AdapterRegistry & registry);

}  // namespace cortex_ros2_bridge::adapters

#endif  // CORTEX_ROS2_BRIDGE__ADAPTERS__TENSOR_HPP_
