// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#include "cortex_ros2_bridge/cortex_to_ros2_node.hpp"

#include <cortex_wire/fingerprint_table.hpp>
#include <rclcpp_components/register_node_macro.hpp>

#include <stdexcept>
#include <string>

#include "cortex_ros2_bridge/adapters/arrays.hpp"
#include "cortex_ros2_bridge/adapters/image.hpp"
#include "cortex_ros2_bridge/adapters/pointcloud.hpp"
#include "cortex_ros2_bridge/adapters/pose.hpp"
#include "cortex_ros2_bridge/adapters/primitives.hpp"
#include "cortex_ros2_bridge/adapters/tensor.hpp"
#include "cortex_ros2_bridge/adapters/transform.hpp"
#include "cortex_ros2_bridge/qos.hpp"

namespace cortex_ros2_bridge
{

namespace
{

// Register the primitive adapters + bindings into the process globals the
// first time we instantiate any bridge component. Idempotent.
void ensure_primitives_registered()
{
  static bool done = []() {
      auto & ar = AdapterRegistry::global();
      auto & br = BindingFactoryRegistry::global();
      adapters::register_primitives(ar);
      adapters::register_array_adapters(ar);
      adapters::register_image_adapters(ar);
      adapters::register_pointcloud_adapters(ar);
      adapters::register_pose_adapters(ar);
      adapters::register_transform_adapters(ar);
      adapters::register_tensor_adapters(ar);
      register_primitive_bindings(br);
      register_standard_bindings(br);
      return true;
    }();
  (void)done;
}

}  // namespace

CortexToRos2Bridge::CortexToRos2Bridge(const rclcpp::NodeOptions & options)
: rclcpp::Node("cortex_to_ros2", options),
  ctx_(std::make_shared<zmq::context_t>(1))
{
  ensure_primitives_registered();
  this->declare_parameter<std::string>("config_path", "");
  initialize();
}

CortexToRos2Bridge::~CortexToRos2Bridge()
{
  for (auto & b : bindings_) {
    if (b) {b->stop();}
  }
  bindings_.clear();
  discovery_.reset();
  if (ctx_) {
    ctx_->shutdown();
    ctx_->close();
  }
}

std::size_t CortexToRos2Bridge::num_active_bindings() const noexcept
{
  return bindings_.size();
}

void CortexToRos2Bridge::initialize()
{
  const auto config_path = this->get_parameter("config_path").as_string();
  if (config_path.empty()) {
    throw std::runtime_error(
            "CortexToRos2Bridge: required parameter 'config_path' is empty");
  }
  config_ = load_config(config_path);

  discovery_ = std::make_unique<cortex_wire::DiscoveryClient>(
    *ctx_, config_.cortex.discovery_address);

  const auto & adapters = AdapterRegistry::global();
  const auto & factories = BindingFactoryRegistry::global();

  for (const auto & entry : config_.entries) {
    if (entry.direction != Direction::CortexToRos2) {continue;}

    const auto * fp_entry = cortex_wire::find_by_name(entry.cortex.type);
    if (!fp_entry) {
      RCLCPP_ERROR(
        get_logger(), "[%s] unknown cortex type '%s' — skipping",
        entry.name.c_str(), entry.cortex.type.c_str());
      continue;
    }
    const auto kind = fp_entry->kind;
    const auto expected_fingerprint = fp_entry->fingerprint;

    // PR4 requires ros2.type to be explicit; defaulting (look up the
    // canonical ROS 2 type for the Cortex kind via the registry) is a
    // follow-up — see PLAN.md §13 follow-ups.
    if (!entry.ros2.type) {
      RCLCPP_ERROR(
        get_logger(),
        "[%s] cortex_to_ros2 entry must specify ros2.type — skipping",
        entry.name.c_str());
      continue;
    }
    const auto & ros2_type_name = *entry.ros2.type;

    if (!adapters.has_cortex_to_ros2(kind, ros2_type_name)) {
      RCLCPP_ERROR(
        get_logger(),
        "[%s] no adapter registered for (%s -> %s)",
        entry.name.c_str(), entry.cortex.type.c_str(), ros2_type_name.c_str());
      continue;
    }
    auto factory = factories.get_cortex_to_ros2(ros2_type_name);
    if (!factory) {
      RCLCPP_ERROR(
        get_logger(),
        "[%s] no binding factory for ros2 type '%s'",
        entry.name.c_str(), ros2_type_name.c_str());
      continue;
    }

    cortex_wire::TopicInfo topic_info;
    try {
      auto lookup = discovery_->lookup(entry.cortex.topic);
      if (!lookup) {
        RCLCPP_ERROR(
          get_logger(),
          "[%s] discovery: topic '%s' not registered — skipping",
          entry.name.c_str(), entry.cortex.topic.c_str());
        continue;
      }
      topic_info = std::move(*lookup);
    } catch (const std::exception & e) {
      RCLCPP_ERROR(
        get_logger(), "[%s] discovery lookup failed: %s — skipping",
        entry.name.c_str(), e.what());
      continue;
    }

    if (topic_info.fingerprint != expected_fingerprint) {
      RCLCPP_ERROR(
        get_logger(),
        "[%s] fingerprint mismatch: daemon=0x%016lx, expected=0x%016lx (for %s) — "
        "refusing to bridge",
        entry.name.c_str(),
        static_cast<unsigned long>(topic_info.fingerprint),
        static_cast<unsigned long>(expected_fingerprint),
        entry.cortex.type.c_str());
      continue;
    }

    auto binding = factory(
      this, ctx_.get(), entry, topic_info, kind, adapters, make_qos(entry.qos));
    if (!binding) {
      RCLCPP_ERROR(
        get_logger(), "[%s] factory returned null binding — skipping",
        entry.name.c_str());
      continue;
    }
    binding->start();
    bindings_.push_back(std::move(binding));
    RCLCPP_INFO(
      get_logger(), "[%s] bridging Cortex(%s) -> ROS2(%s)",
      entry.name.c_str(), entry.cortex.topic.c_str(), entry.ros2.topic.c_str());
  }
}

}  // namespace cortex_ros2_bridge

RCLCPP_COMPONENTS_REGISTER_NODE(cortex_ros2_bridge::CortexToRos2Bridge)
