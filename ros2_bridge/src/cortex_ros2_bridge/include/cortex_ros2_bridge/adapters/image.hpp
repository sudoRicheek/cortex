// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#ifndef CORTEX_ROS2_BRIDGE__ADAPTERS__IMAGE_HPP_
#define CORTEX_ROS2_BRIDGE__ADAPTERS__IMAGE_HPP_

#include <sensor_msgs/msg/image.hpp>

#include "cortex_ros2_bridge/adapter.hpp"
#include "cortex_ros2_bridge/registry.hpp"

namespace cortex_ros2_bridge::adapters
{

// ImageMessage(data: ndarray, encoding: str, width: int, height: int) <->
//   sensor_msgs/msg/Image
//
// Forward path: one memcpy from the ZMQ OOB frame into Image::data (we cannot
// alias the buffer because std::vector<uint8_t>'s sizing constructor value-
// initialises). The frame is then released. See PLAN.md §13.2.
class ImageAdapter : public BidirectionalAdapter<sensor_msgs::msg::Image>
{
public:
  cortex_wire::MessageKind cortex_kind() const override;
  std::string_view ros2_type_name() const override;
  std::unique_ptr<sensor_msgs::msg::Image> to_ros2(const CortexInbound & in) const override;
  CortexOutbound to_cortex(
    const sensor_msgs::msg::Image & msg, std::uint64_t sequence,
    const BridgeEntry & cfg) const override;
};

std::size_t register_image_adapters(AdapterRegistry & registry);

}  // namespace cortex_ros2_bridge::adapters

#endif  // CORTEX_ROS2_BRIDGE__ADAPTERS__IMAGE_HPP_
