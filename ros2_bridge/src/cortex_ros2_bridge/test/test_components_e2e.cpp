// Copyright (c) 2026, Cortex contributors. Apache-2.0.
//
// End-to-end smoke tests for the CortexToRos2Bridge / Ros2ToCortexBridge
// composable nodes. These exercise the full path through:
//   - a hand-rolled REP mock standing in for the Python discovery daemon
//   - a raw zmq PUB/SUB pair playing the role of the Cortex publisher/sub
//   - the real bridge component + a real rclcpp::Node consumer
//
// We avoid launch_testing here so the test stays in-process and self-contained
// (launch_testing brings in a python orchestration layer that is hard to
// wire up cleanly inside ament_add_gtest).
#include "cortex_ros2_bridge/cortex_to_ros2_node.hpp"
#include "cortex_ros2_bridge/ros2_to_cortex_node.hpp"

#include <cortex_wire/discovery_client.hpp>
#include <cortex_wire/fingerprint_table.hpp>
#include <cortex_wire/header.hpp>
#include <cortex_wire/metadata.hpp>

#include <gtest/gtest.h>
#include <msgpack.hpp>
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/string.hpp>
#include <zmq.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace std::chrono_literals;
using cortex_wire::DiscoveryCommand;
using cortex_wire::DiscoveryStatus;

namespace
{

// ---- mock discovery daemon -----------------------------------------------

// A minimal REP responder that mimics the Python discovery daemon: handles
// REGISTER_TOPIC, UNREGISTER_TOPIC, and LOOKUP_TOPIC. State is in-memory.
class MockDiscoveryDaemon
{
public:
  MockDiscoveryDaemon(zmq::context_t & ctx, std::string endpoint)
  : ctx_(ctx), endpoint_(std::move(endpoint))
  {
    socket_ = zmq::socket_t(ctx_, zmq::socket_type::rep);
    socket_.set(zmq::sockopt::linger, 0);
    socket_.set(zmq::sockopt::rcvtimeo, 100);
    socket_.set(zmq::sockopt::sndtimeo, 100);
    socket_.bind(endpoint_);
    thread_ = std::thread(&MockDiscoveryDaemon::run, this);
  }

  ~MockDiscoveryDaemon()
  {
    running_.store(false);
    if (thread_.joinable()) {thread_.join();}
  }

  void register_topic(const cortex_wire::TopicInfo & info)
  {
    std::lock_guard<std::mutex> g(mu_);
    topics_[info.name] = info;
  }

  std::size_t topic_count()
  {
    std::lock_guard<std::mutex> g(mu_);
    return topics_.size();
  }

  bool has_topic(const std::string & name)
  {
    std::lock_guard<std::mutex> g(mu_);
    return topics_.count(name) > 0;
  }

private:
  void run()
  {
    while (running_.load()) {
      zmq::message_t req;
      zmq::recv_result_t r;
      try {
        r = socket_.recv(req, zmq::recv_flags::none);
      } catch (const zmq::error_t &) {
        return;
      }
      if (!r) {continue;}

      const auto reply = handle(req.data(), req.size());
      try {
        zmq::message_t out(reply.data(), reply.size());
        (void)socket_.send(out, zmq::send_flags::none);
      } catch (const zmq::error_t &) {
      }
    }
  }

  std::vector<std::uint8_t> handle(const void * data, std::size_t size)
  {
    msgpack::object_handle oh;
    try {
      oh = msgpack::unpack(static_cast<const char *>(data), size);
    } catch (...) {
      return pack_response(DiscoveryStatus::Error, "bad msgpack");
    }
    const auto & root = oh.get();
    if (root.type != msgpack::type::MAP) {
      return pack_response(DiscoveryStatus::Error, "expected map");
    }

    DiscoveryCommand cmd = DiscoveryCommand::Ping;
    std::string topic_name;
    cortex_wire::TopicInfo info;
    bool have_info = false;
    for (std::uint32_t i = 0; i < root.via.map.size; ++i) {
      const auto & k = root.via.map.ptr[i].key;
      const auto & v = root.via.map.ptr[i].val;
      if (k.type != msgpack::type::STR) {continue;}
      const std::string key(k.via.str.ptr, k.via.str.size);
      if (key == "command" && v.type == msgpack::type::POSITIVE_INTEGER) {
        cmd = static_cast<DiscoveryCommand>(v.via.u64);
      } else if (key == "topic_name" && v.type == msgpack::type::STR) {
        topic_name.assign(v.via.str.ptr, v.via.str.size);
      } else if (key == "topic_info" && v.type == msgpack::type::BIN) {
        info = unpack_topic_info(v.via.bin.ptr, v.via.bin.size);
        have_info = true;
      }
    }

    std::lock_guard<std::mutex> g(mu_);
    switch (cmd) {
      case DiscoveryCommand::RegisterTopic: {
          if (!have_info) {return pack_response(DiscoveryStatus::Error, "no info");}
          topics_[info.name] = info;
          return pack_response(DiscoveryStatus::Ok, "ok");
        }
      case DiscoveryCommand::UnregisterTopic: {
          topics_.erase(topic_name);
          return pack_response(DiscoveryStatus::Ok, "ok");
        }
      case DiscoveryCommand::LookupTopic: {
          auto it = topics_.find(topic_name);
          if (it == topics_.end()) {
            return pack_response(DiscoveryStatus::NotFound, "missing");
          }
          return pack_response(DiscoveryStatus::Ok, "ok", &it->second);
        }
      default:
        return pack_response(DiscoveryStatus::Ok, "ignored");
    }
  }

  static cortex_wire::TopicInfo unpack_topic_info(const void * data, std::size_t size)
  {
    msgpack::object_handle oh = msgpack::unpack(static_cast<const char *>(data), size);
    cortex_wire::TopicInfo info;
    const auto & m = oh.get();
    if (m.type != msgpack::type::MAP) {return info;}
    for (std::uint32_t i = 0; i < m.via.map.size; ++i) {
      const auto & k = m.via.map.ptr[i].key;
      const auto & v = m.via.map.ptr[i].val;
      if (k.type != msgpack::type::STR) {continue;}
      const std::string key(k.via.str.ptr, k.via.str.size);
      if (key == "name" && v.type == msgpack::type::STR) {
        info.name.assign(v.via.str.ptr, v.via.str.size);
      } else if (key == "address" && v.type == msgpack::type::STR) {
        info.address.assign(v.via.str.ptr, v.via.str.size);
      } else if (key == "message_type" && v.type == msgpack::type::STR) {
        info.message_type.assign(v.via.str.ptr, v.via.str.size);
      } else if (key == "fingerprint" && v.type == msgpack::type::POSITIVE_INTEGER) {
        info.fingerprint = v.via.u64;
      } else if (key == "publisher_node" && v.type == msgpack::type::STR) {
        info.publisher_node.assign(v.via.str.ptr, v.via.str.size);
      }
    }
    return info;
  }

  static std::vector<std::uint8_t> pack_response(
    DiscoveryStatus status, const std::string & msg,
    const cortex_wire::TopicInfo * info = nullptr)
  {
    msgpack::sbuffer buf;
    msgpack::packer<msgpack::sbuffer> pk(buf);
    const std::uint32_t map_size = 2 + (info ? 1 : 0);
    pk.pack_map(map_size);
    pk.pack(std::string("status"));
    pk.pack(static_cast<std::int32_t>(status));
    pk.pack(std::string("message"));
    pk.pack(msg);
    if (info) {
      msgpack::sbuffer info_buf;
      msgpack::packer<msgpack::sbuffer> ipk(info_buf);
      ipk.pack_map(5);
      ipk.pack(std::string("name"));            ipk.pack(info->name);
      ipk.pack(std::string("address"));         ipk.pack(info->address);
      ipk.pack(std::string("message_type"));    ipk.pack(info->message_type);
      ipk.pack(std::string("fingerprint"));     ipk.pack(info->fingerprint);
      ipk.pack(std::string("publisher_node"));  ipk.pack(info->publisher_node);
      pk.pack(std::string("topic_info"));
      pk.pack_bin(static_cast<std::uint32_t>(info_buf.size()));
      pk.pack_bin_body(info_buf.data(), info_buf.size());
    }
    return std::vector<std::uint8_t>(
      reinterpret_cast<const std::uint8_t *>(buf.data()),
      reinterpret_cast<const std::uint8_t *>(buf.data()) + buf.size());
  }

  zmq::context_t & ctx_;
  std::string endpoint_;
  zmq::socket_t socket_;
  std::thread thread_;
  std::atomic<bool> running_{true};
  std::mutex mu_;
  std::unordered_map<std::string, cortex_wire::TopicInfo> topics_;
};

// ---- helpers --------------------------------------------------------------

std::string unique_ipc_path()
{
  static std::atomic<int> counter{0};
  const int pid = static_cast<int>(::getpid());
  return "ipc:///tmp/cortex_bridge_test_" + std::to_string(pid) + "_" +
         std::to_string(counter.fetch_add(1));
}

std::string write_temp_yaml(const std::string & body)
{
  // tmpnam is fine for tests — we own the namespace.
  std::string path = std::string(std::tmpnam(nullptr)) + ".yaml";
  std::ofstream f(path);
  f << body;
  return path;
}

// Globals shared by the rclcpp::init / shutdown lifecycle.
struct RclcppEnv : ::testing::Environment
{
  void SetUp() override
  {
    rclcpp::init(0, nullptr);
  }
  void TearDown() override
  {
    rclcpp::shutdown();
  }
};
::testing::Environment * const kRclcppEnv =
  ::testing::AddGlobalTestEnvironment(new RclcppEnv);

// Pack one Cortex multipart message [topic, header, metadata, ...] onto `sock`.
void send_cortex_frame(
  zmq::socket_t & sock, const std::string & topic, std::uint64_t fingerprint,
  std::uint64_t sequence, const std::vector<std::uint8_t> & metadata)
{
  cortex_wire::MessageHeader header{fingerprint, 0, sequence};
  std::array<std::uint8_t, cortex_wire::MessageHeader::kSize> header_bytes{};
  header.to_bytes(header_bytes.data());

  zmq::message_t f0(topic.data(), topic.size());
  zmq::message_t f1(header_bytes.data(), header_bytes.size());
  zmq::message_t f2(metadata.data(), metadata.size());
  (void)sock.send(f0, zmq::send_flags::sndmore);
  (void)sock.send(f1, zmq::send_flags::sndmore);
  (void)sock.send(f2, zmq::send_flags::none);
}

template<typename Pred>
bool wait_for(Pred pred, std::chrono::milliseconds timeout = 2s)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) {return true;}
    std::this_thread::sleep_for(20ms);
  }
  return pred();
}

}  // namespace

// ---- CortexToRos2 end-to-end ----------------------------------------------

TEST(CortexToRos2BridgeE2E, StringMessageFlowsToRosTopic)
{
  // Mock daemon endpoint and a Cortex publisher endpoint.
  zmq::context_t pub_ctx(1);
  const auto daemon_addr = unique_ipc_path();
  const auto pub_addr = unique_ipc_path();
  MockDiscoveryDaemon daemon(pub_ctx, daemon_addr);

  // Pre-register the "publisher" topic so the bridge can look it up.
  const auto * fp = cortex_wire::find_by_name("StringMessage");
  ASSERT_NE(fp, nullptr);
  daemon.register_topic({
        "/test/string", pub_addr, "StringMessage", fp->fingerprint, "test_publisher",
      });

  // Bind a raw PUB socket as the "Cortex publisher".
  zmq::socket_t pub(pub_ctx, zmq::socket_type::pub);
  pub.set(zmq::sockopt::linger, 0);
  pub.bind(pub_addr);

  // Write the bridge YAML.
  const auto config_path = write_temp_yaml(
    "version: 1\n"
    "cortex:\n"
    "  discovery_address: \"" + daemon_addr + "\"\n"
    "cortex_to_ros2:\n"
    "  - name: hello\n"
    "    cortex: {topic: \"/test/string\", type: StringMessage}\n"
    "    ros2:   {topic: \"/ros/hello\", type: \"std_msgs/msg/String\"}\n");

  // Bring up the bridge component.
  rclcpp::NodeOptions opts;
  opts.parameter_overrides({{"config_path", config_path}});
  auto bridge = std::make_shared<cortex_ros2_bridge::CortexToRos2Bridge>(opts);
  ASSERT_EQ(bridge->num_active_bindings(), 1u);

  // Subscribe a Node to the ROS topic.
  auto consumer = std::make_shared<rclcpp::Node>("ros_consumer");
  std::atomic<int> received{0};
  std::string last_payload;
  auto sub = consumer->create_subscription<std_msgs::msg::String>(
    "/ros/hello", rclcpp::QoS(10),
    [&](const std_msgs::msg::String & msg) {
      last_payload = msg.data;
      received.fetch_add(1);
    });

  rclcpp::executors::SingleThreadedExecutor exec;
  exec.add_node(bridge);
  exec.add_node(consumer);

  // Spin in a worker thread while we feed messages.
  std::atomic<bool> spinning{true};
  std::thread spinner([&]() {
        while (spinning.load()) {exec.spin_some(50ms);}
      });

  // Emit one Cortex-format multipart message via the raw PUB socket.
  std::this_thread::sleep_for(200ms);  // let SUB connect
  cortex_wire::MetadataBuilder b(1);
  b.pack_str("hello from cortex");
  auto frames = std::move(b).finish();

  // The bridge SUBs by topic prefix; emit the same topic string here.
  for (int i = 0; i < 5 && received.load() == 0; ++i) {
    send_cortex_frame(pub, "/test/string", fp->fingerprint, i, frames.metadata);
    std::this_thread::sleep_for(100ms);
  }

  EXPECT_TRUE(wait_for([&] {return received.load() > 0;}));
  EXPECT_EQ(last_payload, "hello from cortex");

  spinning.store(false);
  spinner.join();
  std::remove(config_path.c_str());
}

// ---- Ros2ToCortex end-to-end ----------------------------------------------

TEST(Ros2ToCortexBridgeE2E, StringMessageFlowsToCortexSocket)
{
  zmq::context_t sub_ctx(1);
  const auto daemon_addr = unique_ipc_path();
  MockDiscoveryDaemon daemon(sub_ctx, daemon_addr);

  // YAML: a ros2_to_cortex entry that binds to a ROS topic and emits to
  // Cortex topic "/test/echo".
  const auto config_path = write_temp_yaml(
    "version: 1\n"
    "cortex:\n"
    "  discovery_address: \"" + daemon_addr + "\"\n"
    "  node_name_prefix: \"e2e_test\"\n"
    "ros2_to_cortex:\n"
    "  - name: echo\n"
    "    ros2:   {topic: \"/ros/in\", type: \"std_msgs/msg/String\"}\n"
    "    cortex: {topic: \"/test/echo\", type: StringMessage}\n");

  rclcpp::NodeOptions opts;
  opts.parameter_overrides({{"config_path", config_path}});
  auto bridge = std::make_shared<cortex_ros2_bridge::Ros2ToCortexBridge>(opts);
  ASSERT_EQ(bridge->num_active_bindings(), 1u);

  // The bridge should have registered with the daemon.
  EXPECT_TRUE(wait_for([&] {return daemon.has_topic("/test/echo");}));

  // Look up the endpoint the bridge bound to.
  cortex_wire::DiscoveryClient client(sub_ctx, daemon_addr, 1s);
  const auto info = client.lookup("/test/echo");
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(info->message_type, "StringMessage");
  EXPECT_NE(info->fingerprint, 0u);

  // Subscribe to that endpoint with a raw SUB socket.
  zmq::socket_t sub(sub_ctx, zmq::socket_type::sub);
  sub.set(zmq::sockopt::linger, 0);
  sub.set(zmq::sockopt::rcvtimeo, 100);
  sub.connect(info->address);
  sub.set(zmq::sockopt::subscribe, std::string("/test/echo"));

  // Spin the bridge executor and publish a ROS message.
  auto producer = std::make_shared<rclcpp::Node>("ros_producer");
  auto pub = producer->create_publisher<std_msgs::msg::String>(
    "/ros/in", rclcpp::QoS(10));

  rclcpp::executors::SingleThreadedExecutor exec;
  exec.add_node(bridge);
  exec.add_node(producer);

  std::atomic<bool> spinning{true};
  std::thread spinner([&]() {
        while (spinning.load()) {exec.spin_some(20ms);}
      });

  // Give the SUB time to connect to the PUB before we publish.
  std::this_thread::sleep_for(200ms);

  std_msgs::msg::String msg;
  msg.data = "echo me";

  // Retry the publish a few times — SUB connect on IPC can drop the first
  // sends if the SUB hasn't completed its handshake.
  std::vector<zmq::message_t> recv_frames;
  for (int attempt = 0; attempt < 10 && recv_frames.empty(); ++attempt) {
    pub->publish(msg);
    std::this_thread::sleep_for(100ms);

    while (true) {
      zmq::message_t frame;
      zmq::recv_result_t r;
      try {
        r = sub.recv(frame, zmq::recv_flags::none);
      } catch (const zmq::error_t &) {
        break;
      }
      if (!r) {break;}
      const bool more = frame.more();
      recv_frames.emplace_back(std::move(frame));
      if (!more) {break;}
    }
  }

  ASSERT_GE(recv_frames.size(), 3u);

  const auto header = cortex_wire::MessageHeader::from_bytes(
    recv_frames[1].data(), recv_frames[1].size());
  EXPECT_EQ(header.fingerprint, info->fingerprint);

  const auto md = cortex_wire::DecodedMetadata::from_bytes(
    recv_frames[2].data(), recv_frames[2].size());
  ASSERT_EQ(md.field_count(), 1u);
  ASSERT_EQ(md.field(0).type, msgpack::type::STR);
  EXPECT_EQ(
    std::string(md.field(0).via.str.ptr, md.field(0).via.str.size), "echo me");

  spinning.store(false);
  spinner.join();
  std::remove(config_path.c_str());
}
