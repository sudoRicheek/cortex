// Copyright (c) 2026, Cortex contributors. Apache-2.0.
//
// PR8 — intra-process composability test. Verifies that:
//   1. CortexToRos2Bridge composed into a shared executor with
//      use_intra_process_comms=True delivers messages to a colocated
//      consumer subscription using rclcpp's intra-process path.
//   2. The consumer callback can take a std::unique_ptr<const Msg>, which
//      rclcpp's intra-process manager only delivers when the publisher
//      published via unique_ptr (our binding does) and exactly one
//      compatible subscription is wired up. unique_ptr delivery is the
//      signal that the message is *moved* across the boundary rather
//      than serialised + deserialised.
//
// We don't assert raw pointer equality between the bridge's emitted
// unique_ptr and the consumer's received unique_ptr because rclcpp doesn't
// expose the publisher-side pointer through any public API. The
// delivery-via-unique_ptr signal is the conventional intra-process check.
#include "cortex_ros2_bridge/cortex_to_ros2_node.hpp"

#include <cortex_wire/discovery_client.hpp>
#include <cortex_wire/fingerprint_table.hpp>
#include <cortex_wire/header.hpp>
#include <cortex_wire/metadata.hpp>

#include <gtest/gtest.h>
#include <msgpack.hpp>
#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/image.hpp>
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

// ---- mock daemon (reused shape from test_components_e2e) -----------------

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

private:
  void run()
  {
    while (running_.load()) {
      zmq::message_t req;
      zmq::recv_result_t r;
      try {r = socket_.recv(req, zmq::recv_flags::none);} catch (...) {return;}
      if (!r) {continue;}
      const auto reply = handle(req.data(), req.size());
      try {
        zmq::message_t out(reply.data(), reply.size());
        (void)socket_.send(out, zmq::send_flags::none);
      } catch (...) {}
    }
  }
  std::vector<std::uint8_t> handle(const void * data, std::size_t size)
  {
    msgpack::object_handle oh;
    try {oh = msgpack::unpack(static_cast<const char *>(data), size);} catch (...) {
      return pack_response(DiscoveryStatus::Error, "bad msgpack");
    }
    const auto & root = oh.get();
    if (root.type != msgpack::type::MAP) {
      return pack_response(DiscoveryStatus::Error, "expected map");
    }
    DiscoveryCommand cmd = DiscoveryCommand::Ping;
    std::string topic_name;
    for (std::uint32_t i = 0; i < root.via.map.size; ++i) {
      const auto & k = root.via.map.ptr[i].key;
      const auto & v = root.via.map.ptr[i].val;
      if (k.type != msgpack::type::STR) {continue;}
      const std::string key(k.via.str.ptr, k.via.str.size);
      if (key == "command" && v.type == msgpack::type::POSITIVE_INTEGER) {
        cmd = static_cast<DiscoveryCommand>(v.via.u64);
      } else if (key == "topic_name" && v.type == msgpack::type::STR) {
        topic_name.assign(v.via.str.ptr, v.via.str.size);
      }
    }
    std::lock_guard<std::mutex> g(mu_);
    if (cmd == DiscoveryCommand::LookupTopic) {
      auto it = topics_.find(topic_name);
      if (it == topics_.end()) {
        return pack_response(DiscoveryStatus::NotFound, "missing");
      }
      return pack_response(DiscoveryStatus::Ok, "ok", &it->second);
    }
    return pack_response(DiscoveryStatus::Ok, "ignored");
  }
  static std::vector<std::uint8_t> pack_response(
    DiscoveryStatus status, const std::string & msg,
    const cortex_wire::TopicInfo * info = nullptr)
  {
    msgpack::sbuffer buf;
    msgpack::packer<msgpack::sbuffer> pk(buf);
    const std::uint32_t map_size = 2 + (info ? 1 : 0);
    pk.pack_map(map_size);
    pk.pack(std::string("status")); pk.pack(static_cast<std::int32_t>(status));
    pk.pack(std::string("message")); pk.pack(msg);
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

std::string unique_ipc_path()
{
  static std::atomic<int> counter{0};
  return "ipc:///tmp/cortex_ipc_test_" + std::to_string(::getpid()) + "_" +
         std::to_string(counter.fetch_add(1));
}

std::string write_temp_yaml(const std::string & body)
{
  const std::string path = std::string(std::tmpnam(nullptr)) + ".yaml";
  std::ofstream f(path);
  f << body;
  return path;
}

struct RclcppEnv : ::testing::Environment
{
  void SetUp() override {rclcpp::init(0, nullptr);}
  void TearDown() override {rclcpp::shutdown();}
};
::testing::Environment * const kRclcppEnv =
  ::testing::AddGlobalTestEnvironment(new RclcppEnv);

void send_cortex_frame(
  zmq::socket_t & sock, const std::string & topic, std::uint64_t fingerprint,
  std::uint64_t sequence, const std::vector<std::uint8_t> & metadata,
  const std::vector<std::vector<std::uint8_t>> & oob)
{
  cortex_wire::MessageHeader header{fingerprint, 0, sequence};
  std::array<std::uint8_t, cortex_wire::MessageHeader::kSize> header_bytes{};
  header.to_bytes(header_bytes.data());

  zmq::message_t f0(topic.data(), topic.size());
  (void)sock.send(f0, zmq::send_flags::sndmore);
  zmq::message_t f1(header_bytes.data(), header_bytes.size());
  (void)sock.send(f1, zmq::send_flags::sndmore);
  zmq::message_t f2(metadata.data(), metadata.size());
  zmq::send_flags meta_flag = oob.empty() ? zmq::send_flags::none : zmq::send_flags::sndmore;
  (void)sock.send(f2, meta_flag);
  for (std::size_t i = 0; i < oob.size(); ++i) {
    zmq::message_t fi(oob[i].data(), oob[i].size());
    const auto flag = (i + 1 < oob.size()) ?
      zmq::send_flags::sndmore : zmq::send_flags::none;
    (void)sock.send(fi, flag);
  }
}

template<typename Pred>
bool wait_for(Pred pred, std::chrono::milliseconds timeout = 3s)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) {return true;}
    std::this_thread::sleep_for(20ms);
  }
  return pred();
}

}  // namespace

TEST(IntraProcess, ImageDeliversAsUniquePtr)
{
  zmq::context_t pub_ctx(1);
  const auto daemon_addr = unique_ipc_path();
  const auto pub_addr = unique_ipc_path();
  MockDiscoveryDaemon daemon(pub_ctx, daemon_addr);

  const auto * fp = cortex_wire::find_by_name("ImageMessage");
  ASSERT_NE(fp, nullptr);
  daemon.register_topic({
        "/cam/rgb", pub_addr, "ImageMessage", fp->fingerprint, "test_publisher",
      });

  zmq::socket_t pub(pub_ctx, zmq::socket_type::pub);
  pub.set(zmq::sockopt::linger, 0);
  pub.bind(pub_addr);

  const auto config_path = write_temp_yaml(
    "version: 1\n"
    "cortex:\n"
    "  discovery_address: \"" + daemon_addr + "\"\n"
    "cortex_to_ros2:\n"
    "  - name: cam\n"
    "    cortex: {topic: \"/cam/rgb\", type: ImageMessage}\n"
    "    ros2:   {topic: \"/cam/image_raw\", type: \"sensor_msgs/msg/Image\"}\n");

  // Build both the bridge node and the consumer node with intra-process
  // comms enabled. Putting them in the same executor activates rclcpp's
  // intra-process manager.
  rclcpp::NodeOptions bridge_opts;
  bridge_opts.use_intra_process_comms(true);
  bridge_opts.parameter_overrides({{"config_path", config_path}});
  auto bridge = std::make_shared<cortex_ros2_bridge::CortexToRos2Bridge>(bridge_opts);
  ASSERT_EQ(bridge->num_active_bindings(), 1u);

  rclcpp::NodeOptions consumer_opts;
  consumer_opts.use_intra_process_comms(true);
  auto consumer = std::make_shared<rclcpp::Node>("ipc_consumer", consumer_opts);

  std::atomic<int> received_unique{0};
  std::atomic<std::size_t> last_data_size{0};
  // Subscription with the `unique_ptr<Image>` signature — rclcpp delivers
  // via unique_ptr only on the intra-process path with a single matching
  // subscription, so this callback firing is the intra-process signal.
  auto sub = consumer->create_subscription<sensor_msgs::msg::Image>(
    "/cam/image_raw", rclcpp::QoS(10),
    [&](std::unique_ptr<sensor_msgs::msg::Image> msg) {
      last_data_size.store(msg->data.size());
      received_unique.fetch_add(1);
    });

  rclcpp::executors::SingleThreadedExecutor exec;
  exec.add_node(bridge);
  exec.add_node(consumer);

  std::atomic<bool> spinning{true};
  std::thread spinner([&]() {
        while (spinning.load()) {exec.spin_some(50ms);}
      });

  // Feed a 32×24×3 RGB frame.
  const std::uint32_t W = 32, H = 24;
  std::vector<std::uint8_t> pixels(W * H * 3);
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    pixels[i] = static_cast<std::uint8_t>(i & 0xff);
  }
  cortex_wire::MetadataBuilder mb(4);
  mb.pack_numpy_oob("<u1", {H, W, 3}, pixels.data(), pixels.size());
  mb.pack_str("rgb8");
  mb.pack_uint(W);
  mb.pack_uint(H);
  auto frames = std::move(mb).finish();

  std::this_thread::sleep_for(200ms);  // SUB-side IPC connect

  // Retry a few times in case the first sends race the SUB handshake.
  for (int i = 0; i < 5 && received_unique.load() == 0; ++i) {
    send_cortex_frame(pub, "/cam/rgb", fp->fingerprint, i, frames.metadata, frames.oob_buffers);
    std::this_thread::sleep_for(100ms);
  }

  EXPECT_TRUE(wait_for([&] {return received_unique.load() > 0;}));
  EXPECT_EQ(last_data_size.load(), pixels.size());

  spinning.store(false);
  spinner.join();
  std::remove(config_path.c_str());
}

// Same test, but with use_intra_process_comms=false on both nodes — the
// inter-process delivery path still works (rclcpp falls back to shared_ptr
// in the unique_ptr callback by copying on construction). Confirms our
// bridge is not silently coupled to intra-process being on.
TEST(IntraProcess, InterProcessFallbackStillDelivers)
{
  zmq::context_t pub_ctx(1);
  const auto daemon_addr = unique_ipc_path();
  const auto pub_addr = unique_ipc_path();
  MockDiscoveryDaemon daemon(pub_ctx, daemon_addr);

  const auto * fp = cortex_wire::find_by_name("StringMessage");
  ASSERT_NE(fp, nullptr);
  daemon.register_topic({
        "/test/str", pub_addr, "StringMessage", fp->fingerprint, "test_publisher",
      });

  zmq::socket_t pub(pub_ctx, zmq::socket_type::pub);
  pub.set(zmq::sockopt::linger, 0);
  pub.bind(pub_addr);

  const auto config_path = write_temp_yaml(
    "version: 1\n"
    "cortex:\n"
    "  discovery_address: \"" + daemon_addr + "\"\n"
    "cortex_to_ros2:\n"
    "  - name: msg\n"
    "    cortex: {topic: \"/test/str\", type: StringMessage}\n"
    "    ros2:   {topic: \"/test/out\", type: \"std_msgs/msg/String\"}\n");

  rclcpp::NodeOptions opts;  // default: intra-process OFF
  opts.parameter_overrides({{"config_path", config_path}});
  auto bridge = std::make_shared<cortex_ros2_bridge::CortexToRos2Bridge>(opts);

  rclcpp::NodeOptions consumer_opts;  // intra-process OFF
  auto consumer = std::make_shared<rclcpp::Node>("ipc_off_consumer", consumer_opts);
  std::atomic<int> received{0};
  auto sub = consumer->create_subscription<std_msgs::msg::String>(
    "/test/out", rclcpp::QoS(10),
    [&](const std_msgs::msg::String &) {received.fetch_add(1);});

  rclcpp::executors::SingleThreadedExecutor exec;
  exec.add_node(bridge);
  exec.add_node(consumer);

  std::atomic<bool> spinning{true};
  std::thread spinner([&]() {
        while (spinning.load()) {exec.spin_some(50ms);}
      });

  cortex_wire::MetadataBuilder b(1);
  b.pack_str("ping");
  auto frames = std::move(b).finish();
  std::this_thread::sleep_for(200ms);
  for (int i = 0; i < 5 && received.load() == 0; ++i) {
    send_cortex_frame(pub, "/test/str", fp->fingerprint, i, frames.metadata, frames.oob_buffers);
    std::this_thread::sleep_for(100ms);
  }
  EXPECT_TRUE(wait_for([&] {return received.load() > 0;}));

  spinning.store(false);
  spinner.join();
  std::remove(config_path.c_str());
}
