// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#ifndef CORTEX_ROS2_BRIDGE__CONFIG_HPP_
#define CORTEX_ROS2_BRIDGE__CONFIG_HPP_

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace cortex_ros2_bridge
{

enum class Reliability { Reliable, BestEffort };
enum class Durability { Volatile, TransientLocal };
enum class History { KeepLast, KeepAll };
enum class LoanedMessagesMode { Auto, Force, Never };
enum class Direction { CortexToRos2, Ros2ToCortex };

struct QosSettings
{
  Reliability reliability = Reliability::Reliable;
  Durability durability = Durability::Volatile;
  History history = History::KeepLast;
  std::uint32_t depth = 10;
};

struct CortexSettings
{
  std::string discovery_address = "ipc:///tmp/cortex_discovery";
  std::string node_name_prefix = "cortex_bridge";
};

struct DefaultsSettings
{
  QosSettings qos;
  LoanedMessagesMode loaned_messages = LoanedMessagesMode::Auto;
};

struct CortexEndpoint
{
  std::string topic;
  std::string type;  // Cortex message class name, e.g. "ImageMessage"
};

struct Ros2Endpoint
{
  std::string topic;
  // Empty / nullopt → adapter chooses the default ROS 2 type for the Cortex type.
  std::optional<std::string> type;
  // Static override; if unset, adapter uses the message's own frame_id field
  // (when present) or the empty string.
  std::optional<std::string> frame_id;
  // For PoseMessage / TransformMessage: also publish to /tf using tf2.
  bool broadcast_tf = false;
  std::optional<std::string> tf_parent;
};

struct BridgeEntry
{
  std::string name;
  Direction direction;
  CortexEndpoint cortex;
  Ros2Endpoint ros2;
  QosSettings qos;  // defaults already merged
};

struct BridgeConfig
{
  int version = 1;
  CortexSettings cortex;
  DefaultsSettings defaults;
  std::vector<BridgeEntry> entries;
};

class ConfigError : public std::runtime_error
{
public:
  using std::runtime_error::runtime_error;
};

// Parse YAML text into a validated BridgeConfig. Throws ConfigError on any
// schema violation or YAML syntax error.
BridgeConfig parse_config(const std::string & yaml_text);

// Load YAML from a file. Throws ConfigError if the file cannot be opened, or
// for any error parse_config would raise.
BridgeConfig load_config(const std::string & path);

// Helpers for diagnostics / logging.
const char * to_string(Reliability v);
const char * to_string(Durability v);
const char * to_string(History v);
const char * to_string(LoanedMessagesMode v);
const char * to_string(Direction v);

}  // namespace cortex_ros2_bridge

#endif  // CORTEX_ROS2_BRIDGE__CONFIG_HPP_
