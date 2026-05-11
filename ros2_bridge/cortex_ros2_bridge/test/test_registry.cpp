// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#include "cortex_ros2_bridge/registry.hpp"

#include <gtest/gtest.h>
#include <std_msgs/msg/int64.hpp>
#include <std_msgs/msg/string.hpp>

#include <memory>

using cortex_ros2_bridge::AdapterRegistry;
using cortex_ros2_bridge::BidirectionalAdapter;
using cortex_ros2_bridge::CortexInbound;
using cortex_ros2_bridge::CortexOutbound;
using cortex_ros2_bridge::CortexToRos2Adapter;
using cortex_ros2_bridge::Ros2ToCortexAdapter;
using cortex_wire::MessageKind;

namespace
{

class FakeStringAdapter : public BidirectionalAdapter<std_msgs::msg::String>
{
public:
  cortex_wire::MessageKind cortex_kind() const override {return MessageKind::StringMessage;}
  std::string_view ros2_type_name() const override {return "std_msgs/msg/String";}
  std::unique_ptr<std_msgs::msg::String> to_ros2(const CortexInbound &) const override
  {
    auto m = std::make_unique<std_msgs::msg::String>();
    m->data = "from_cortex";
    return m;
  }
  CortexOutbound to_cortex(
    const std_msgs::msg::String &, std::uint64_t,
    const cortex_ros2_bridge::BridgeEntry &) const override
  {
    return CortexOutbound{};
  }
};

class FakeIntAdapter : public BidirectionalAdapter<std_msgs::msg::Int64>
{
public:
  cortex_wire::MessageKind cortex_kind() const override {return MessageKind::IntMessage;}
  std::string_view ros2_type_name() const override {return "std_msgs/msg/Int64";}
  std::unique_ptr<std_msgs::msg::Int64> to_ros2(const CortexInbound &) const override
  {
    return std::make_unique<std_msgs::msg::Int64>();
  }
  CortexOutbound to_cortex(
    const std_msgs::msg::Int64 &, std::uint64_t,
    const cortex_ros2_bridge::BridgeEntry &) const override
  {
    return CortexOutbound{};
  }
};

}  // namespace

TEST(Registry, RegisterAndLookupBidirectional)
{
  AdapterRegistry reg;
  EXPECT_TRUE(reg.register_bidirectional<std_msgs::msg::String>(
      std::make_shared<FakeStringAdapter>()));
  EXPECT_TRUE(reg.has_cortex_to_ros2(MessageKind::StringMessage, "std_msgs/msg/String"));
  EXPECT_TRUE(reg.has_ros2_to_cortex(MessageKind::StringMessage, "std_msgs/msg/String"));

  auto fwd = reg.find_cortex_to_ros2<std_msgs::msg::String>(
    MessageKind::StringMessage, "std_msgs/msg/String");
  ASSERT_NE(fwd, nullptr);
  cortex_ros2_bridge::BridgeEntry cfg;
  cortex_wire::DecodedMetadata empty = []() {
    msgpack::sbuffer s; msgpack::packer<msgpack::sbuffer> p(s); p.pack_array(0);
    return cortex_wire::DecodedMetadata::from_bytes(s.data(), s.size());
  }();
  std::vector<cortex_wire::ZmqFramePtr> no_oob;
  cortex_wire::MessageHeader hdr{};
  CortexInbound in{hdr, empty, no_oob, cfg};
  auto msg = fwd->to_ros2(in);
  ASSERT_NE(msg, nullptr);
  EXPECT_EQ(msg->data, "from_cortex");
}

TEST(Registry, MismatchedRos2TypeReturnsNull)
{
  AdapterRegistry reg;
  reg.register_bidirectional<std_msgs::msg::String>(std::make_shared<FakeStringAdapter>());

  // Look up with the wrong template type but the right (kind, type_name).
  // The typeid check inside find_* must catch this.
  auto wrong = reg.find_cortex_to_ros2<std_msgs::msg::Int64>(
    MessageKind::StringMessage, "std_msgs/msg/String");
  EXPECT_EQ(wrong, nullptr);
}

TEST(Registry, DistinctRos2TypesShareSameCortexKind)
{
  // Two adapters claiming the same Cortex kind but different ROS 2 types
  // must coexist — the kind is not unique in the registry, the (kind,
  // type_name) pair is.
  AdapterRegistry reg;

  class A : public BidirectionalAdapter<std_msgs::msg::String>
  {
public:
    cortex_wire::MessageKind cortex_kind() const override
    {
      return MessageKind::HeaderMessage;
    }
    std::string_view ros2_type_name() const override {return "std_msgs/msg/String";}
    std::unique_ptr<std_msgs::msg::String> to_ros2(const CortexInbound &) const override
    {
      return std::make_unique<std_msgs::msg::String>();
    }
    CortexOutbound to_cortex(
      const std_msgs::msg::String &, std::uint64_t,
      const cortex_ros2_bridge::BridgeEntry &) const override
    {
      return CortexOutbound{};
    }
  };
  class B : public BidirectionalAdapter<std_msgs::msg::Int64>
  {
public:
    cortex_wire::MessageKind cortex_kind() const override
    {
      return MessageKind::HeaderMessage;
    }
    std::string_view ros2_type_name() const override {return "std_msgs/msg/Int64";}
    std::unique_ptr<std_msgs::msg::Int64> to_ros2(const CortexInbound &) const override
    {
      return std::make_unique<std_msgs::msg::Int64>();
    }
    CortexOutbound to_cortex(
      const std_msgs::msg::Int64 &, std::uint64_t,
      const cortex_ros2_bridge::BridgeEntry &) const override
    {
      return CortexOutbound{};
    }
  };

  EXPECT_TRUE(reg.register_bidirectional<std_msgs::msg::String>(std::make_shared<A>()));
  EXPECT_TRUE(reg.register_bidirectional<std_msgs::msg::Int64>(std::make_shared<B>()));

  EXPECT_NE(
    reg.find_cortex_to_ros2<std_msgs::msg::String>(
      MessageKind::HeaderMessage, "std_msgs/msg/String"),
    nullptr);
  EXPECT_NE(
    reg.find_cortex_to_ros2<std_msgs::msg::Int64>(
      MessageKind::HeaderMessage, "std_msgs/msg/Int64"),
    nullptr);
}

TEST(Registry, DuplicateRegistrationRejected)
{
  AdapterRegistry reg;
  EXPECT_TRUE(reg.register_bidirectional<std_msgs::msg::String>(
      std::make_shared<FakeStringAdapter>()));
  // Second registration with the same key returns false on each direction;
  // register_bidirectional returns false because at least one failed.
  EXPECT_FALSE(reg.register_bidirectional<std_msgs::msg::String>(
      std::make_shared<FakeStringAdapter>()));
}

TEST(Registry, MissingLookupReturnsNull)
{
  AdapterRegistry reg;
  EXPECT_FALSE(reg.has_cortex_to_ros2(MessageKind::ImageMessage, "sensor_msgs/msg/Image"));
  auto p = reg.find_cortex_to_ros2<std_msgs::msg::String>(
    MessageKind::ImageMessage, "sensor_msgs/msg/Image");
  EXPECT_EQ(p, nullptr);
}

TEST(Registry, GlobalIsSharedAcrossCalls)
{
  // Sanity: AdapterRegistry::global() returns the same instance.
  EXPECT_EQ(&AdapterRegistry::global(), &AdapterRegistry::global());
}
