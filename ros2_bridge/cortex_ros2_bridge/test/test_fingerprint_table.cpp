// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#include "cortex_ros2_bridge/cortex_wire/fingerprint_table.hpp"

#include <gtest/gtest.h>

#include <set>
#include <string_view>

using cortex_ros2_bridge::cortex_wire::find_by_fingerprint;
using cortex_ros2_bridge::cortex_wire::find_by_name;
using cortex_ros2_bridge::cortex_wire::kFingerprintTable;
using cortex_ros2_bridge::cortex_wire::kFingerprintTableSize;
using cortex_ros2_bridge::cortex_wire::MessageKind;
using cortex_ros2_bridge::cortex_wire::to_string;

TEST(FingerprintTable, HasExpectedSize)
{
  // 16 standard catalogue messages as of 2026-05. If this drifts, regenerate
  // the header with scripts/gen_fingerprint_table.py.
  EXPECT_EQ(kFingerprintTableSize, 16u);
}

TEST(FingerprintTable, FingerprintsAreUnique)
{
  std::set<std::uint64_t> seen;
  for (std::size_t i = 0; i < kFingerprintTableSize; ++i) {
    EXPECT_TRUE(seen.insert(kFingerprintTable[i].fingerprint).second)
      << "duplicate fingerprint at index " << i;
  }
}

TEST(FingerprintTable, NamesAreUnique)
{
  std::set<std::string_view> seen;
  for (std::size_t i = 0; i < kFingerprintTableSize; ++i) {
    EXPECT_TRUE(seen.insert(kFingerprintTable[i].name).second)
      << "duplicate name at index " << i;
  }
}

TEST(FingerprintTable, KnownEntriesLookup)
{
  // These fingerprints come directly from running the Python cortex tree;
  // they form the canonical proof that the generator produces the same
  // 64-bit hashes the daemon and publishers emit. Updating any of these
  // means the wire is incompatible — bump a version, don't just change
  // the constant.
  const auto * image = find_by_fingerprint(0xa51dd7f890942cadULL);
  ASSERT_NE(image, nullptr);
  EXPECT_EQ(image->kind, MessageKind::ImageMessage);
  EXPECT_EQ(image->name, "ImageMessage");

  const auto * pose = find_by_fingerprint(0xa50e8b029e07992fULL);
  ASSERT_NE(pose, nullptr);
  EXPECT_EQ(pose->kind, MessageKind::PoseMessage);

  const auto * pc = find_by_fingerprint(0xbef60c17034e979aULL);
  ASSERT_NE(pc, nullptr);
  EXPECT_EQ(pc->kind, MessageKind::PointCloudMessage);
}

TEST(FingerprintTable, FindByName)
{
  const auto * entry = find_by_name("TimestampMessage");
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->fingerprint, 0xf4484907f9c22ee1ULL);
}

TEST(FingerprintTable, UnknownLookupsReturnNull)
{
  EXPECT_EQ(find_by_fingerprint(0xdeadbeefdeadbeefULL), nullptr);
  EXPECT_EQ(find_by_name("NotAMessageType"), nullptr);
}

TEST(FingerprintTable, ToStringWorksForEveryKind)
{
  for (std::size_t i = 0; i < kFingerprintTableSize; ++i) {
    const auto s = to_string(kFingerprintTable[i].kind);
    EXPECT_EQ(s, kFingerprintTable[i].name);
  }
}

TEST(FingerprintTable, QualifiedNamesMatchPython)
{
  // The qualified name encodes module + qualname exactly as Python sees it.
  // This is what the fingerprint hash depends on, so a regression here is a
  // clear sign the generator broke.
  const auto * image = find_by_name("ImageMessage");
  ASSERT_NE(image, nullptr);
  EXPECT_EQ(image->qualified_name, "cortex.messages.standard.ImageMessage");
}
