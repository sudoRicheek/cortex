// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#ifndef CORTEX_ROS2_BRIDGE__ADAPTERS__POINTCLOUD_HPP_
#define CORTEX_ROS2_BRIDGE__ADAPTERS__POINTCLOUD_HPP_

#include <sensor_msgs/msg/point_cloud2.hpp>

#include "cortex_ros2_bridge/adapter.hpp"
#include "cortex_ros2_bridge/registry.hpp"

namespace cortex_ros2_bridge::adapters
{

// PointCloudMessage(points, colors, intensity, normals, frame_id) <->
//   sensor_msgs/msg/PointCloud2
//
// v1 maps points (N×3 float32) <-> PointCloud2 with x/y/z PointFields. The
// optional `colors` / `intensity` / `normals` channels round-trip when set
// — colors as rgb (UINT8x3 packed into 4 bytes with one byte of padding),
// intensity and normals as float32. Missing channels round-trip as msgpack
// nil on the Cortex side and are simply omitted from PointCloud2.fields on
// the ROS side.
class PointCloudAdapter : public BidirectionalAdapter<sensor_msgs::msg::PointCloud2>
{
public:
  cortex_wire::MessageKind cortex_kind() const override;
  std::string_view ros2_type_name() const override;
  std::unique_ptr<sensor_msgs::msg::PointCloud2> to_ros2(
    const CortexInbound & in) const override;
  CortexOutbound to_cortex(
    const sensor_msgs::msg::PointCloud2 & msg, std::uint64_t sequence,
    const BridgeEntry & cfg) const override;
};

std::size_t register_pointcloud_adapters(AdapterRegistry & registry);

}  // namespace cortex_ros2_bridge::adapters

#endif  // CORTEX_ROS2_BRIDGE__ADAPTERS__POINTCLOUD_HPP_
