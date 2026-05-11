// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#include "cortex_ros2_bridge/config.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>

namespace
{

using cortex_ros2_bridge::BridgeConfig;
using cortex_ros2_bridge::ConfigError;
using cortex_ros2_bridge::Direction;
using cortex_ros2_bridge::Durability;
using cortex_ros2_bridge::History;
using cortex_ros2_bridge::LoanedMessagesMode;
using cortex_ros2_bridge::load_config;
using cortex_ros2_bridge::parse_config;
using cortex_ros2_bridge::Reliability;

constexpr const char * kMinimal = R"(
version: 1
cortex_to_ros2:
  - name: img
    cortex:
      topic: "/cam/rgb"
      type: ImageMessage
    ros2:
      topic: "/camera/image_raw"
)";

}  // namespace

TEST(ConfigParse, MinimalValid)
{
  const auto cfg = parse_config(kMinimal);
  EXPECT_EQ(cfg.version, 1);
  EXPECT_EQ(cfg.cortex.discovery_address, "ipc:///tmp/cortex_discovery");
  EXPECT_EQ(cfg.cortex.node_name_prefix, "cortex_bridge");
  ASSERT_EQ(cfg.entries.size(), 1u);
  const auto & e = cfg.entries[0];
  EXPECT_EQ(e.name, "img");
  EXPECT_EQ(e.direction, Direction::CortexToRos2);
  EXPECT_EQ(e.cortex.topic, "/cam/rgb");
  EXPECT_EQ(e.cortex.type, "ImageMessage");
  EXPECT_EQ(e.ros2.topic, "/camera/image_raw");
  EXPECT_FALSE(e.ros2.type.has_value());
  EXPECT_FALSE(e.ros2.frame_id.has_value());
  EXPECT_FALSE(e.ros2.broadcast_tf);
  // qos defaults
  EXPECT_EQ(e.qos.reliability, Reliability::Reliable);
  EXPECT_EQ(e.qos.durability, Durability::Volatile);
  EXPECT_EQ(e.qos.history, History::KeepLast);
  EXPECT_EQ(e.qos.depth, 10u);
  EXPECT_EQ(cfg.defaults.loaned_messages, LoanedMessagesMode::Auto);
}

TEST(ConfigParse, DefaultsMergeWithPerEntryOverrides)
{
  const std::string yaml = R"(
version: 1
cortex:
  discovery_address: "ipc:///tmp/disc"
  node_name_prefix: "br"
defaults:
  qos:
    reliability: best_effort
    durability: transient_local
    history: keep_all
    depth: 5
  loaned_messages: force
cortex_to_ros2:
  - name: a
    cortex: {topic: "/a", type: ArrayMessage}
    ros2:   {topic: "/a"}
  - name: b
    cortex: {topic: "/b", type: ArrayMessage}
    ros2:   {topic: "/b"}
    qos:
      reliability: reliable
      depth: 1
)";
  const auto cfg = parse_config(yaml);
  EXPECT_EQ(cfg.cortex.discovery_address, "ipc:///tmp/disc");
  EXPECT_EQ(cfg.cortex.node_name_prefix, "br");
  EXPECT_EQ(cfg.defaults.loaned_messages, LoanedMessagesMode::Force);
  ASSERT_EQ(cfg.entries.size(), 2u);

  EXPECT_EQ(cfg.entries[0].qos.reliability, Reliability::BestEffort);
  EXPECT_EQ(cfg.entries[0].qos.durability, Durability::TransientLocal);
  EXPECT_EQ(cfg.entries[0].qos.history, History::KeepAll);
  EXPECT_EQ(cfg.entries[0].qos.depth, 5u);

  // Per-entry override only flips the named fields; other defaults persist.
  EXPECT_EQ(cfg.entries[1].qos.reliability, Reliability::Reliable);
  EXPECT_EQ(cfg.entries[1].qos.durability, Durability::TransientLocal);
  EXPECT_EQ(cfg.entries[1].qos.history, History::KeepAll);
  EXPECT_EQ(cfg.entries[1].qos.depth, 1u);
}

TEST(ConfigParse, BothDirections)
{
  const std::string yaml = R"(
version: 1
cortex_to_ros2:
  - name: out
    cortex: {topic: "/out", type: PoseMessage}
    ros2:   {topic: "/out"}
ros2_to_cortex:
  - name: in
    cortex: {topic: "/in", type: PoseMessage}
    ros2:   {topic: "/in", type: "geometry_msgs/msg/Twist"}
)";
  const auto cfg = parse_config(yaml);
  ASSERT_EQ(cfg.entries.size(), 2u);
  EXPECT_EQ(cfg.entries[0].direction, Direction::CortexToRos2);
  EXPECT_EQ(cfg.entries[1].direction, Direction::Ros2ToCortex);
  ASSERT_TRUE(cfg.entries[1].ros2.type.has_value());
  EXPECT_EQ(*cfg.entries[1].ros2.type, "geometry_msgs/msg/Twist");
}

TEST(ConfigParse, TfBroadcastFields)
{
  const std::string yaml = R"(
version: 1
cortex_to_ros2:
  - name: pose
    cortex: {topic: "/state/pose", type: PoseMessage}
    ros2:
      topic: "/robot/pose"
      frame_id: "base_link"
      broadcast_tf: true
      tf_parent: "map"
)";
  const auto cfg = parse_config(yaml);
  ASSERT_EQ(cfg.entries.size(), 1u);
  const auto & r = cfg.entries[0].ros2;
  EXPECT_TRUE(r.broadcast_tf);
  ASSERT_TRUE(r.frame_id.has_value());
  EXPECT_EQ(*r.frame_id, "base_link");
  ASSERT_TRUE(r.tf_parent.has_value());
  EXPECT_EQ(*r.tf_parent, "map");
}

TEST(ConfigParse, MissingVersionFails)
{
  const std::string yaml = R"(
cortex_to_ros2:
  - name: x
    cortex: {topic: "/x", type: PoseMessage}
    ros2:   {topic: "/x"}
)";
  EXPECT_THROW(parse_config(yaml), ConfigError);
}

TEST(ConfigParse, UnsupportedVersionFails)
{
  EXPECT_THROW(parse_config("version: 2\ncortex_to_ros2: []\n"), ConfigError);
}

TEST(ConfigParse, EmptyEntriesFails)
{
  EXPECT_THROW(parse_config("version: 1\n"), ConfigError);
  EXPECT_THROW(
    parse_config("version: 1\ncortex_to_ros2: []\nros2_to_cortex: []\n"), ConfigError);
}

TEST(ConfigParse, DuplicateNamesAcrossDirectionsFails)
{
  const std::string yaml = R"(
version: 1
cortex_to_ros2:
  - name: dup
    cortex: {topic: "/a", type: PoseMessage}
    ros2:   {topic: "/a"}
ros2_to_cortex:
  - name: dup
    cortex: {topic: "/b", type: PoseMessage}
    ros2:   {topic: "/b", type: "geometry_msgs/msg/PoseStamped"}
)";
  EXPECT_THROW(parse_config(yaml), ConfigError);
}

TEST(ConfigParse, InvalidReliabilityFails)
{
  const std::string yaml = R"(
version: 1
defaults:
  qos:
    reliability: maybe
cortex_to_ros2:
  - name: x
    cortex: {topic: "/x", type: PoseMessage}
    ros2:   {topic: "/x"}
)";
  EXPECT_THROW(parse_config(yaml), ConfigError);
}

TEST(ConfigParse, InvalidLoanedModeFails)
{
  const std::string yaml = R"(
version: 1
defaults:
  loaned_messages: sometimes
cortex_to_ros2:
  - name: x
    cortex: {topic: "/x", type: PoseMessage}
    ros2:   {topic: "/x"}
)";
  EXPECT_THROW(parse_config(yaml), ConfigError);
}

TEST(ConfigParse, BroadcastTfRequiresTfParent)
{
  const std::string yaml = R"(
version: 1
cortex_to_ros2:
  - name: x
    cortex: {topic: "/x", type: PoseMessage}
    ros2:
      topic: "/x"
      broadcast_tf: true
)";
  EXPECT_THROW(parse_config(yaml), ConfigError);
}

TEST(ConfigParse, MissingRequiredFieldFails)
{
  // cortex.type missing
  const std::string yaml = R"(
version: 1
cortex_to_ros2:
  - name: x
    cortex: {topic: "/x"}
    ros2:   {topic: "/x"}
)";
  EXPECT_THROW(parse_config(yaml), ConfigError);
}

TEST(ConfigParse, Ros2ToCortexRequiresRos2Type)
{
  const std::string yaml = R"(
version: 1
ros2_to_cortex:
  - name: x
    cortex: {topic: "/x", type: PoseMessage}
    ros2:   {topic: "/x"}
)";
  EXPECT_THROW(parse_config(yaml), ConfigError);
}

TEST(ConfigParse, EmptyStringsRejected)
{
  const std::string yaml = R"(
version: 1
cortex_to_ros2:
  - name: ""
    cortex: {topic: "/x", type: PoseMessage}
    ros2:   {topic: "/x"}
)";
  EXPECT_THROW(parse_config(yaml), ConfigError);
}

TEST(ConfigParse, MalformedYamlFails)
{
  EXPECT_THROW(parse_config("version: 1\nfoo: [unterminated"), ConfigError);
}

TEST(ConfigParse, NonMappingRootFails)
{
  EXPECT_THROW(parse_config("- 1\n- 2\n"), ConfigError);
}

TEST(ConfigParse, ListIsNotSequenceFails)
{
  const std::string yaml = R"(
version: 1
cortex_to_ros2:
  name: oops
)";
  EXPECT_THROW(parse_config(yaml), ConfigError);
}

TEST(ConfigParse, ErrorMessageIncludesPath)
{
  const std::string yaml = R"(
version: 1
cortex_to_ros2:
  - name: x
    cortex: {topic: "/x"}
    ros2:   {topic: "/x"}
)";
  try {
    parse_config(yaml);
    FAIL() << "expected ConfigError";
  } catch (const ConfigError & e) {
    const std::string msg = e.what();
    EXPECT_NE(msg.find("cortex_to_ros2[0]"), std::string::npos) << msg;
    EXPECT_NE(msg.find("type"), std::string::npos) << msg;
  }
}

TEST(ConfigLoad, FileRoundTrip)
{
  // Write the minimal YAML to a temp file and load it.
  const std::string path = std::string(std::tmpnam(nullptr)) + ".yaml";
  {
    std::ofstream f(path);
    ASSERT_TRUE(f.good());
    f << kMinimal;
  }
  const auto cfg = load_config(path);
  EXPECT_EQ(cfg.entries.size(), 1u);
  EXPECT_EQ(cfg.entries[0].cortex.topic, "/cam/rgb");
  std::remove(path.c_str());
}

TEST(ConfigLoad, MissingFileFails)
{
  EXPECT_THROW(
    load_config("/nonexistent/cortex_ros2_bridge/should_not_exist.yaml"), ConfigError);
}
