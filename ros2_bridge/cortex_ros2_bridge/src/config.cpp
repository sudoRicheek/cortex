// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#include "cortex_ros2_bridge/config.hpp"

#include <yaml-cpp/yaml.h>

#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>

namespace cortex_ros2_bridge
{

namespace
{

std::string join_path(const std::string & parent, const std::string & key)
{
  return parent.empty() ? key : parent + "." + key;
}

template <typename T>
T required_scalar(const YAML::Node & node, const std::string & key, const std::string & parent)
{
  const std::string field = join_path(parent, key);
  if (!node[key]) {
    throw ConfigError(field + ": required field is missing");
  }
  try {
    return node[key].as<T>();
  } catch (const YAML::Exception &) {
    throw ConfigError(field + ": invalid value type");
  }
}

template <typename T>
std::optional<T> optional_scalar(
  const YAML::Node & node, const std::string & key, const std::string & parent)
{
  if (!node[key]) {
    return std::nullopt;
  }
  const std::string field = join_path(parent, key);
  try {
    return node[key].as<T>();
  } catch (const YAML::Exception &) {
    throw ConfigError(field + ": invalid value type");
  }
}

Reliability parse_reliability(const std::string & s, const std::string & path)
{
  if (s == "reliable") return Reliability::Reliable;
  if (s == "best_effort") return Reliability::BestEffort;
  throw ConfigError(
    path + ": invalid reliability '" + s + "' (expected 'reliable' or 'best_effort')");
}

Durability parse_durability(const std::string & s, const std::string & path)
{
  if (s == "volatile") return Durability::Volatile;
  if (s == "transient_local") return Durability::TransientLocal;
  throw ConfigError(
    path + ": invalid durability '" + s + "' (expected 'volatile' or 'transient_local')");
}

History parse_history(const std::string & s, const std::string & path)
{
  if (s == "keep_last") return History::KeepLast;
  if (s == "keep_all") return History::KeepAll;
  throw ConfigError(path + ": invalid history '" + s + "' (expected 'keep_last' or 'keep_all')");
}

LoanedMessagesMode parse_loaned(const std::string & s, const std::string & path)
{
  if (s == "auto") return LoanedMessagesMode::Auto;
  if (s == "force") return LoanedMessagesMode::Force;
  if (s == "never") return LoanedMessagesMode::Never;
  throw ConfigError(
    path + ": invalid loaned_messages '" + s + "' (expected 'auto', 'force', or 'never')");
}

void require_non_empty(const std::string & value, const std::string & path)
{
  if (value.empty()) {
    throw ConfigError(path + ": must be a non-empty string");
  }
}

void apply_qos_overrides(QosSettings & out, const YAML::Node & node, const std::string & path)
{
  if (!node) {
    return;
  }
  if (!node.IsMap()) {
    throw ConfigError(path + ": expected mapping");
  }
  if (auto v = optional_scalar<std::string>(node, "reliability", path)) {
    out.reliability = parse_reliability(*v, join_path(path, "reliability"));
  }
  if (auto v = optional_scalar<std::string>(node, "durability", path)) {
    out.durability = parse_durability(*v, join_path(path, "durability"));
  }
  if (auto v = optional_scalar<std::string>(node, "history", path)) {
    out.history = parse_history(*v, join_path(path, "history"));
  }
  if (auto v = optional_scalar<std::uint32_t>(node, "depth", path)) {
    out.depth = *v;
  }
}

BridgeEntry parse_entry(
  const YAML::Node & node, Direction direction, const QosSettings & defaults_qos,
  const std::string & path)
{
  if (!node.IsMap()) {
    throw ConfigError(path + ": expected mapping");
  }

  BridgeEntry e;
  e.direction = direction;
  e.name = required_scalar<std::string>(node, "name", path);
  require_non_empty(e.name, join_path(path, "name"));

  const auto cortex_node = node["cortex"];
  if (!cortex_node || !cortex_node.IsMap()) {
    throw ConfigError(join_path(path, "cortex") + ": required mapping");
  }
  const std::string cortex_path = join_path(path, "cortex");
  e.cortex.topic = required_scalar<std::string>(cortex_node, "topic", cortex_path);
  e.cortex.type = required_scalar<std::string>(cortex_node, "type", cortex_path);
  require_non_empty(e.cortex.topic, join_path(cortex_path, "topic"));
  require_non_empty(e.cortex.type, join_path(cortex_path, "type"));

  const auto ros2_node = node["ros2"];
  if (!ros2_node || !ros2_node.IsMap()) {
    throw ConfigError(join_path(path, "ros2") + ": required mapping");
  }
  const std::string ros2_path = join_path(path, "ros2");
  e.ros2.topic = required_scalar<std::string>(ros2_node, "topic", ros2_path);
  require_non_empty(e.ros2.topic, join_path(ros2_path, "topic"));
  e.ros2.type = optional_scalar<std::string>(ros2_node, "type", ros2_path);
  e.ros2.frame_id = optional_scalar<std::string>(ros2_node, "frame_id", ros2_path);
  e.ros2.broadcast_tf =
    optional_scalar<bool>(ros2_node, "broadcast_tf", ros2_path).value_or(false);
  e.ros2.tf_parent = optional_scalar<std::string>(ros2_node, "tf_parent", ros2_path);

  if (e.ros2.broadcast_tf && !e.ros2.tf_parent) {
    throw ConfigError(ros2_path + ": broadcast_tf=true requires tf_parent");
  }

  // ros2→cortex needs an explicit ROS 2 type — the adapter cannot infer it
  // from a Cortex type alone in this direction (multiple ROS types map to the
  // same Cortex type, e.g. Twist and PoseStamped both -> PoseMessage).
  if (direction == Direction::Ros2ToCortex && !e.ros2.type) {
    throw ConfigError(ros2_path + ".type: required for ros2_to_cortex entries");
  }

  e.qos = defaults_qos;
  apply_qos_overrides(e.qos, node["qos"], join_path(path, "qos"));

  return e;
}

void check_unique_names(const std::vector<BridgeEntry> & entries)
{
  std::unordered_set<std::string> seen;
  seen.reserve(entries.size());
  for (const auto & e : entries) {
    if (!seen.insert(e.name).second) {
      throw ConfigError("duplicate bridge entry name: '" + e.name + "'");
    }
  }
}

}  // namespace

BridgeConfig parse_config(const std::string & yaml_text)
{
  YAML::Node root;
  try {
    root = YAML::Load(yaml_text);
  } catch (const YAML::Exception & e) {
    throw ConfigError(std::string("YAML parse error: ") + e.what());
  }
  if (!root || root.IsNull()) {
    throw ConfigError("root: empty configuration");
  }
  if (!root.IsMap()) {
    throw ConfigError("root: expected mapping");
  }

  BridgeConfig cfg;

  const int version = required_scalar<int>(root, "version", "");
  if (version != 1) {
    throw ConfigError(
      "version: unsupported value " + std::to_string(version) + " (only 1 is supported)");
  }
  cfg.version = version;

  if (const auto cortex = root["cortex"]) {
    if (!cortex.IsMap()) {
      throw ConfigError("cortex: expected mapping");
    }
    if (auto v = optional_scalar<std::string>(cortex, "discovery_address", "cortex")) {
      require_non_empty(*v, "cortex.discovery_address");
      cfg.cortex.discovery_address = *v;
    }
    if (auto v = optional_scalar<std::string>(cortex, "node_name_prefix", "cortex")) {
      require_non_empty(*v, "cortex.node_name_prefix");
      cfg.cortex.node_name_prefix = *v;
    }
  }

  if (const auto defaults = root["defaults"]) {
    if (!defaults.IsMap()) {
      throw ConfigError("defaults: expected mapping");
    }
    apply_qos_overrides(cfg.defaults.qos, defaults["qos"], "defaults.qos");
    if (auto v = optional_scalar<std::string>(defaults, "loaned_messages", "defaults")) {
      cfg.defaults.loaned_messages = parse_loaned(*v, "defaults.loaned_messages");
    }
  }

  auto parse_list = [&](const char * key, Direction dir) {
    const auto list = root[key];
    if (!list) {
      return;
    }
    if (!list.IsSequence()) {
      throw ConfigError(std::string(key) + ": expected sequence");
    }
    for (std::size_t i = 0; i < list.size(); ++i) {
      const std::string path = std::string(key) + "[" + std::to_string(i) + "]";
      cfg.entries.push_back(parse_entry(list[i], dir, cfg.defaults.qos, path));
    }
  };

  parse_list("cortex_to_ros2", Direction::CortexToRos2);
  parse_list("ros2_to_cortex", Direction::Ros2ToCortex);

  if (cfg.entries.empty()) {
    throw ConfigError(
      "no bridge entries: at least one of 'cortex_to_ros2' or 'ros2_to_cortex' must be non-empty");
  }
  check_unique_names(cfg.entries);

  return cfg;
}

BridgeConfig load_config(const std::string & path)
{
  std::ifstream f(path);
  if (!f) {
    throw ConfigError("cannot open config file: " + path);
  }
  std::stringstream ss;
  ss << f.rdbuf();
  if (f.bad()) {
    throw ConfigError("error reading config file: " + path);
  }
  return parse_config(ss.str());
}

const char * to_string(Reliability v)
{
  switch (v) {
    case Reliability::Reliable: return "reliable";
    case Reliability::BestEffort: return "best_effort";
  }
  return "?";
}

const char * to_string(Durability v)
{
  switch (v) {
    case Durability::Volatile: return "volatile";
    case Durability::TransientLocal: return "transient_local";
  }
  return "?";
}

const char * to_string(History v)
{
  switch (v) {
    case History::KeepLast: return "keep_last";
    case History::KeepAll: return "keep_all";
  }
  return "?";
}

const char * to_string(LoanedMessagesMode v)
{
  switch (v) {
    case LoanedMessagesMode::Auto: return "auto";
    case LoanedMessagesMode::Force: return "force";
    case LoanedMessagesMode::Never: return "never";
  }
  return "?";
}

const char * to_string(Direction v)
{
  switch (v) {
    case Direction::CortexToRos2: return "cortex_to_ros2";
    case Direction::Ros2ToCortex: return "ros2_to_cortex";
  }
  return "?";
}

}  // namespace cortex_ros2_bridge
