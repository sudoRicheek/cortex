// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#include "cortex_ros2_bridge/cortex_wire/discovery_client.hpp"

#include <gtest/gtest.h>
#include <msgpack.hpp>
#include <zmq.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using cortex_ros2_bridge::cortex_wire::DiscoveryClient;
using cortex_ros2_bridge::cortex_wire::DiscoveryCommand;
using cortex_ros2_bridge::cortex_wire::DiscoveryError;
using cortex_ros2_bridge::cortex_wire::DiscoveryStatus;
using cortex_ros2_bridge::cortex_wire::TopicInfo;

namespace
{

// Pack a DiscoveryResponse map the same way the Python daemon does.
std::vector<std::uint8_t> pack_response(
  DiscoveryStatus status, const std::string & message,
  const std::optional<TopicInfo> & topic_info = std::nullopt)
{
  msgpack::sbuffer buf;
  msgpack::packer<msgpack::sbuffer> pk(buf);
  const std::uint32_t map_size = 2 + (topic_info.has_value() ? 1 : 0);
  pk.pack_map(map_size);
  pk.pack(std::string("status")); pk.pack(static_cast<std::int32_t>(status));
  pk.pack(std::string("message")); pk.pack(message);
  if (topic_info) {
    // Pack TopicInfo as a separate msgpack blob (matches Python behaviour).
    msgpack::sbuffer info_buf;
    msgpack::packer<msgpack::sbuffer> info_pk(info_buf);
    info_pk.pack_map(5);
    info_pk.pack(std::string("name")); info_pk.pack(topic_info->name);
    info_pk.pack(std::string("address")); info_pk.pack(topic_info->address);
    info_pk.pack(std::string("message_type")); info_pk.pack(topic_info->message_type);
    info_pk.pack(std::string("fingerprint")); info_pk.pack(topic_info->fingerprint);
    info_pk.pack(std::string("publisher_node")); info_pk.pack(topic_info->publisher_node);

    pk.pack(std::string("topic_info"));
    pk.pack_bin(static_cast<std::uint32_t>(info_buf.size()));
    pk.pack_bin_body(info_buf.data(), info_buf.size());
  }
  return std::vector<std::uint8_t>(
    reinterpret_cast<const std::uint8_t *>(buf.data()),
    reinterpret_cast<const std::uint8_t *>(buf.data()) + buf.size());
}

// A tiny mock REP daemon. Records the last request bytes and replies with a
// canned response.
class MockDaemon
{
public:
  MockDaemon(zmq::context_t & ctx, const std::string & address)
  : socket_(ctx, zmq::socket_type::rep)
  {
    socket_.set(zmq::sockopt::linger, 0);
    socket_.set(zmq::sockopt::rcvtimeo, 2000);
    socket_.set(zmq::sockopt::sndtimeo, 2000);
    socket_.bind(address);
  }

  void serve_once(const std::vector<std::uint8_t> & reply)
  {
    thread_ = std::thread(
      [this, reply]() {
        zmq::message_t req;
        const auto got = socket_.recv(req, zmq::recv_flags::none);
        if (!got) {
          return;
        }
        last_request_.assign(
          static_cast<const std::uint8_t *>(req.data()),
          static_cast<const std::uint8_t *>(req.data()) + req.size());
        zmq::message_t rep(reply.data(), reply.size());
        (void)socket_.send(rep, zmq::send_flags::none);
      });
  }

  ~MockDaemon()
  {
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  std::vector<std::uint8_t> last_request_;

private:
  zmq::socket_t socket_;
  std::thread thread_;
};

std::string unique_endpoint()
{
  // inproc:// for unit tests avoids real filesystem state and is faster.
  static std::atomic<int> counter{0};
  return "inproc://discovery_test_" + std::to_string(counter.fetch_add(1));
}

}  // namespace

TEST(DiscoveryEncode, RequestShapeMatchesPython)
{
  // Verify the encoded request decodes back to the expected map shape (a
  // crude golden test against the protocol; the daemon-side test is in the
  // Python repo).
  const auto bytes = DiscoveryClient::encode_request(
    DiscoveryCommand::LookupTopic, std::string("/foo"), std::nullopt);
  auto oh = msgpack::unpack(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  const auto & root = oh.get();
  ASSERT_EQ(root.type, msgpack::type::MAP);
  // Expected: {command, topic_name}
  EXPECT_EQ(root.via.map.size, 2u);
}

TEST(DiscoveryEncode, RegisterIncludesTopicInfoBlob)
{
  TopicInfo info{"/cam", "ipc:///tmp/x", "ImageMessage", 0xa51dd7f890942cadULL, "cam_node"};
  const auto bytes = DiscoveryClient::encode_request(
    DiscoveryCommand::RegisterTopic, std::nullopt, info);
  auto oh = msgpack::unpack(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  const auto & root = oh.get();
  ASSERT_EQ(root.type, msgpack::type::MAP);
  EXPECT_EQ(root.via.map.size, 3u);

  // Find the topic_info bin and re-decode the embedded TopicInfo.
  bool found = false;
  for (std::uint32_t i = 0; i < root.via.map.size; ++i) {
    const auto & k = root.via.map.ptr[i].key;
    const auto & v = root.via.map.ptr[i].val;
    if (k.type == msgpack::type::STR &&
      std::string_view(k.via.str.ptr, k.via.str.size) == "topic_info")
    {
      ASSERT_EQ(v.type, msgpack::type::BIN);
      auto info_oh = msgpack::unpack(v.via.bin.ptr, v.via.bin.size);
      ASSERT_EQ(info_oh.get().type, msgpack::type::MAP);
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);
}

TEST(DiscoveryDecode, OkResponseWithTopicInfo)
{
  TopicInfo info{"/cam", "ipc:///tmp/cam_pub", "ImageMessage", 0xdeadbeef, "publisher"};
  const auto bytes = pack_response(DiscoveryStatus::Ok, "ok", info);
  const auto resp = DiscoveryClient::decode_response(bytes.data(), bytes.size());
  EXPECT_EQ(resp.status, DiscoveryStatus::Ok);
  ASSERT_TRUE(resp.topic_info.has_value());
  EXPECT_EQ(resp.topic_info->name, "/cam");
  EXPECT_EQ(resp.topic_info->address, "ipc:///tmp/cam_pub");
  EXPECT_EQ(resp.topic_info->message_type, "ImageMessage");
  EXPECT_EQ(resp.topic_info->fingerprint, 0xdeadbeefULL);
  EXPECT_EQ(resp.topic_info->publisher_node, "publisher");
}

TEST(DiscoveryDecode, NotFoundStatusParsed)
{
  const auto bytes = pack_response(DiscoveryStatus::NotFound, "topic not registered");
  const auto resp = DiscoveryClient::decode_response(bytes.data(), bytes.size());
  EXPECT_EQ(resp.status, DiscoveryStatus::NotFound);
  EXPECT_FALSE(resp.topic_info.has_value());
}

TEST(DiscoveryDecode, MissingStatusRejected)
{
  msgpack::sbuffer buf;
  msgpack::packer<msgpack::sbuffer> pk(buf);
  pk.pack_map(1);
  pk.pack(std::string("message"));
  pk.pack(std::string("oops"));
  EXPECT_THROW(
    DiscoveryClient::decode_response(buf.data(), buf.size()), DiscoveryError);
}

TEST(DiscoveryClient, LookupReturnsTopicInfo)
{
  zmq::context_t ctx(1);
  const auto endpoint = unique_endpoint();
  MockDaemon daemon(ctx, endpoint);

  TopicInfo info{"/lidar", "ipc:///tmp/lidar_pub", "PointCloudMessage",
    0xbef60c17034e979aULL, "lidar_node"};
  daemon.serve_once(pack_response(DiscoveryStatus::Ok, "ok", info));

  DiscoveryClient client(ctx, endpoint, std::chrono::milliseconds(2000));
  const auto got = client.lookup("/lidar");
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->address, "ipc:///tmp/lidar_pub");
  EXPECT_EQ(got->fingerprint, 0xbef60c17034e979aULL);
}

TEST(DiscoveryClient, LookupReturnsNullOptOnNotFound)
{
  zmq::context_t ctx(1);
  const auto endpoint = unique_endpoint();
  MockDaemon daemon(ctx, endpoint);
  daemon.serve_once(pack_response(DiscoveryStatus::NotFound, "missing"));

  DiscoveryClient client(ctx, endpoint, std::chrono::milliseconds(2000));
  EXPECT_FALSE(client.lookup("/nope").has_value());
}

TEST(DiscoveryClient, LookupTimeoutThrows)
{
  zmq::context_t ctx(1);
  const auto endpoint = unique_endpoint();

  // Bind a REP that never replies, so recv hits its timeout.
  zmq::socket_t silent(ctx, zmq::socket_type::rep);
  silent.set(zmq::sockopt::linger, 0);
  silent.bind(endpoint);

  DiscoveryClient client(ctx, endpoint, std::chrono::milliseconds(150));
  EXPECT_THROW(client.lookup("/anything"), DiscoveryError);
}
