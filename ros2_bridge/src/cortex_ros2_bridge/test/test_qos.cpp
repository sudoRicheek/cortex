// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#include "cortex_ros2_bridge/qos.hpp"

#include <gtest/gtest.h>

using cortex_ros2_bridge::Durability;
using cortex_ros2_bridge::History;
using cortex_ros2_bridge::QosSettings;
using cortex_ros2_bridge::Reliability;
using cortex_ros2_bridge::make_qos;

TEST(MakeQos, DefaultsMapToReliableVolatileKeepLast)
{
  QosSettings q;
  const auto qos = make_qos(q);
  const auto p = qos.get_rmw_qos_profile();
  EXPECT_EQ(p.reliability, RMW_QOS_POLICY_RELIABILITY_RELIABLE);
  EXPECT_EQ(p.durability, RMW_QOS_POLICY_DURABILITY_VOLATILE);
  EXPECT_EQ(p.history, RMW_QOS_POLICY_HISTORY_KEEP_LAST);
  EXPECT_EQ(p.depth, 10u);
}

TEST(MakeQos, BestEffortIsApplied)
{
  QosSettings q;
  q.reliability = Reliability::BestEffort;
  const auto qos = make_qos(q);
  EXPECT_EQ(
    qos.get_rmw_qos_profile().reliability, RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT);
}

TEST(MakeQos, TransientLocalIsApplied)
{
  QosSettings q;
  q.durability = Durability::TransientLocal;
  const auto qos = make_qos(q);
  EXPECT_EQ(
    qos.get_rmw_qos_profile().durability, RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL);
}

TEST(MakeQos, KeepAllHistory)
{
  QosSettings q;
  q.history = History::KeepAll;
  const auto qos = make_qos(q);
  EXPECT_EQ(
    qos.get_rmw_qos_profile().history, RMW_QOS_POLICY_HISTORY_KEEP_ALL);
}

TEST(MakeQos, DepthIsPropagated)
{
  QosSettings q;
  q.depth = 7;
  const auto qos = make_qos(q);
  EXPECT_EQ(qos.get_rmw_qos_profile().depth, 7u);
}
