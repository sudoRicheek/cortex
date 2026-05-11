// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#include "cortex_ros2_bridge/ros2_to_cortex_node.hpp"

#include <cortex_wire/fingerprint_table.hpp>
#include <rclcpp_components/register_node_macro.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

#include "cortex_ros2_bridge/adapters/primitives.hpp"
#include "cortex_ros2_bridge/qos.hpp"

namespace cortex_ros2_bridge
{

namespace
{

void ensure_primitives_registered()
{
  static bool done = []() {
      adapters::register_primitives(AdapterRegistry::global());
      register_primitive_bindings(BindingFactoryRegistry::global());
      return true;
    }();
  (void)done;
}

// Build a deterministic IPC endpoint for a bridge entry. The slugify pass
// makes the path filesystem-friendly: any character that isn't alnum, dash
// or underscore becomes underscore. Two bridge processes with identical
// configs on the same host would collide here — that's already a bug at the
// config level, and the bind() call will surface it loudly.
std::string slugify(std::string s)
{
  for (auto & c : s) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') {
      c = '_';
    }
  }
  return s;
}

}  // namespace

std::string Ros2ToCortexBridge::make_pub_endpoint(const std::string & entry_name) const
{
  return "ipc:///tmp/cortex/topics/" +
         slugify(config_.cortex.node_name_prefix) + "__" + slugify(entry_name);
}

Ros2ToCortexBridge::Ros2ToCortexBridge(const rclcpp::NodeOptions & options)
: rclcpp::Node("ros2_to_cortex", options),
  ctx_(std::make_shared<zmq::context_t>(1))
{
  ensure_primitives_registered();
  this->declare_parameter<std::string>("config_path", "");
  initialize();
}

Ros2ToCortexBridge::~Ros2ToCortexBridge()
{
  // Stop binding callbacks first — drops the rclcpp subscription so no more
  // ROS messages can race the unregister.
  for (auto & b : bindings_) {
    if (b) {b->stop();}
  }
  bindings_.clear();

  if (discovery_) {
    for (const auto & topic : registered_topics_) {
      try {
        discovery_->unregister_topic(topic);
      } catch (const std::exception & e) {
        RCLCPP_WARN(
          get_logger(), "discovery unregister('%s') failed: %s — continuing",
          topic.c_str(), e.what());
      }
    }
  }
  registered_topics_.clear();
  discovery_.reset();

  if (ctx_) {
    ctx_->shutdown();
    ctx_->close();
  }
}

std::size_t Ros2ToCortexBridge::num_active_bindings() const noexcept
{
  return bindings_.size();
}

void Ros2ToCortexBridge::initialize()
{
  const auto config_path = this->get_parameter("config_path").as_string();
  if (config_path.empty()) {
    throw std::runtime_error(
            "Ros2ToCortexBridge: required parameter 'config_path' is empty");
  }
  config_ = load_config(config_path);

  discovery_ = std::make_unique<cortex_wire::DiscoveryClient>(
    *ctx_, config_.cortex.discovery_address);

  const auto & adapters = AdapterRegistry::global();
  const auto & factories = BindingFactoryRegistry::global();

  for (const auto & entry : config_.entries) {
    if (entry.direction != Direction::Ros2ToCortex) {continue;}

    const auto * fp_entry = cortex_wire::find_by_name(entry.cortex.type);
    if (!fp_entry) {
      RCLCPP_ERROR(
        get_logger(), "[%s] unknown cortex type '%s' — skipping",
        entry.name.c_str(), entry.cortex.type.c_str());
      continue;
    }
    const auto kind = fp_entry->kind;
    const auto fingerprint = fp_entry->fingerprint;

    // ros2.type is required for this direction by the config loader (see
    // §5.2 / config.cpp). Defensive recheck:
    if (!entry.ros2.type) {
      RCLCPP_ERROR(
        get_logger(),
        "[%s] ros2_to_cortex entries must specify ros2.type — skipping",
        entry.name.c_str());
      continue;
    }
    const auto & ros2_type_name = *entry.ros2.type;

    if (!adapters.has_ros2_to_cortex(kind, ros2_type_name)) {
      RCLCPP_ERROR(
        get_logger(),
        "[%s] no adapter registered for (%s <- %s)",
        entry.name.c_str(), entry.cortex.type.c_str(), ros2_type_name.c_str());
      continue;
    }
    auto factory = factories.get_ros2_to_cortex(ros2_type_name);
    if (!factory) {
      RCLCPP_ERROR(
        get_logger(),
        "[%s] no binding factory for ros2 type '%s'",
        entry.name.c_str(), ros2_type_name.c_str());
      continue;
    }

    const auto pub_endpoint = make_pub_endpoint(entry.name);

    // ZMQ's ipc:// bind() requires the parent directory to exist.
    // make_pub_endpoint() puts sockets under /tmp/cortex/topics/; create it
    // if missing. Stripping the "ipc://" prefix gives the filesystem path.
    constexpr std::string_view kIpcPrefix = "ipc://";
    if (pub_endpoint.rfind(kIpcPrefix, 0) == 0) {
      const std::filesystem::path sock_path(pub_endpoint.substr(kIpcPrefix.size()));
      std::error_code ec;
      std::filesystem::create_directories(sock_path.parent_path(), ec);
      if (ec) {
        RCLCPP_ERROR(
          get_logger(),
          "[%s] cannot create parent dir for %s: %s — skipping",
          entry.name.c_str(), pub_endpoint.c_str(), ec.message().c_str());
        continue;
      }
    }

    auto binding = factory(
      this, ctx_.get(), entry, kind, fingerprint, pub_endpoint, adapters, make_qos(entry.qos));
    if (!binding) {
      RCLCPP_ERROR(
        get_logger(), "[%s] factory returned null binding — skipping",
        entry.name.c_str());
      continue;
    }

    // Register with discovery so Cortex subscribers can find this endpoint.
    cortex_wire::TopicInfo info{
      entry.cortex.topic,
      pub_endpoint,
      entry.cortex.type,
      fingerprint,
      this->get_fully_qualified_name(),
    };
    try {
      discovery_->register_topic(info);
      registered_topics_.push_back(entry.cortex.topic);
    } catch (const std::exception & e) {
      RCLCPP_ERROR(
        get_logger(),
        "[%s] discovery register('%s') failed: %s — dropping binding",
        entry.name.c_str(), entry.cortex.topic.c_str(), e.what());
      // Let `binding` destruct here, which closes the PUB socket.
      continue;
    }

    binding->start();
    bindings_.push_back(std::move(binding));
    RCLCPP_INFO(
      get_logger(),
      "[%s] bridging ROS2(%s) -> Cortex(%s @ %s)",
      entry.name.c_str(), entry.ros2.topic.c_str(),
      entry.cortex.topic.c_str(), pub_endpoint.c_str());
  }
}

}  // namespace cortex_ros2_bridge

RCLCPP_COMPONENTS_REGISTER_NODE(cortex_ros2_bridge::Ros2ToCortexBridge)
