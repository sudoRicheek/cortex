// End-to-end Publisher/Subscriber round-trip test.
//
// Two modes of operation are covered:
//   1. Direct constructor (no discovery) — Subscriber connects to a known
//      endpoint, exercises the recv-thread, fingerprint check, and OOB-bearing
//      payload.
//   2. Convenience factory `Subscriber::connect()` — backed by a tiny mock
//      discovery REP daemon, exercises the lookup-via-discovery path.
//
// We deliberately mock the discovery daemon in-process rather than depending
// on `cortex-discovery` being on PATH.
#include "cortex_wire/context.hpp"
#include "cortex_wire/discovery_client.hpp"
#include "cortex_wire/metadata.hpp"
#include "cortex_wire/publisher.hpp"
#include "cortex_wire/subscriber.hpp"

#include <gtest/gtest.h>
#include <msgpack.hpp>
#include <zmq.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

using cortex_wire::Context;
using cortex_wire::DiscoveryClient;
using cortex_wire::DiscoveryCommand;
using cortex_wire::DiscoveryStatus;
using cortex_wire::MetadataBuilder;
using cortex_wire::Publisher;
using cortex_wire::Subscriber;
using cortex_wire::TopicInfo;

// A tiny in-process discovery daemon. Speaks the same REQ/REP protocol the
// real daemon does, just enough for register + lookup. Multi-request loop so
// it can serve a register-then-lookup sequence.
class MockDaemon
{
public:
  MockDaemon(zmq::context_t & ctx, const std::string & address)
  : socket_(ctx, zmq::socket_type::rep)
  {
    socket_.set(zmq::sockopt::linger, 0);
    socket_.set(zmq::sockopt::rcvtimeo, 100);
    socket_.set(zmq::sockopt::sndtimeo, 2000);
    socket_.bind(address);
    running_.store(true);
    thread_ = std::thread(&MockDaemon::loop, this);
  }

  ~MockDaemon()
  {
    running_.store(false);
    if (thread_.joinable()) {thread_.join();}
  }

  std::optional<TopicInfo> get(const std::string & name)
  {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = topics_.find(name);
    if (it == topics_.end()) {return std::nullopt;}
    return it->second;
  }

private:
  void loop()
  {
    while (running_.load()) {
      zmq::message_t req;
      auto r = socket_.recv(req, zmq::recv_flags::none);
      if (!r) {continue;}
      respond(req);
    }
  }

  void respond(const zmq::message_t & req)
  {
    msgpack::object_handle oh;
    try {
      oh = msgpack::unpack(static_cast<const char *>(req.data()), req.size());
    } catch (...) {
      send_status(DiscoveryStatus::Error, "bad msgpack");
      return;
    }
    const auto & root = oh.get();
    if (root.type != msgpack::type::MAP) {
      send_status(DiscoveryStatus::Error, "not a map");
      return;
    }

    std::int32_t cmd = 0;
    std::string topic_name;
    std::optional<TopicInfo> info;
    for (std::uint32_t i = 0; i < root.via.map.size; ++i) {
      const auto & k = root.via.map.ptr[i].key;
      const auto & v = root.via.map.ptr[i].val;
      std::string_view key(k.via.str.ptr, k.via.str.size);
      if (key == "command") {cmd = static_cast<std::int32_t>(v.via.i64);}
      if (key == "topic_name" && v.type == msgpack::type::STR) {
        topic_name.assign(v.via.str.ptr, v.via.str.size);
      }
      if (key == "topic_info" && v.type == msgpack::type::BIN) {
        info = decode_topic_info(v.via.bin.ptr, v.via.bin.size);
      }
    }

    if (cmd == static_cast<int>(DiscoveryCommand::RegisterTopic) && info) {
      std::lock_guard<std::mutex> lk(mu_);
      topics_[info->name] = *info;
      send_status(DiscoveryStatus::Ok, "ok");
      return;
    }
    if (cmd == static_cast<int>(DiscoveryCommand::UnregisterTopic)) {
      std::lock_guard<std::mutex> lk(mu_);
      topics_.erase(topic_name);
      send_status(DiscoveryStatus::Ok, "ok");
      return;
    }
    if (cmd == static_cast<int>(DiscoveryCommand::LookupTopic)) {
      std::optional<TopicInfo> found;
      {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = topics_.find(topic_name);
        if (it != topics_.end()) {found = it->second;}
      }
      if (!found) {
        send_status(DiscoveryStatus::NotFound, "topic not registered");
      } else {
        send_topic_info(DiscoveryStatus::Ok, "ok", *found);
      }
      return;
    }
    send_status(DiscoveryStatus::Error, "unsupported command");
  }

  static TopicInfo decode_topic_info(const char * data, std::size_t size)
  {
    auto oh = msgpack::unpack(data, size);
    const auto & m = oh.get();
    TopicInfo info{};
    for (std::uint32_t i = 0; i < m.via.map.size; ++i) {
      const auto & k = m.via.map.ptr[i].key;
      const auto & v = m.via.map.ptr[i].val;
      std::string_view key(k.via.str.ptr, k.via.str.size);
      if (key == "name") {info.name.assign(v.via.str.ptr, v.via.str.size);}
      else if (key == "address") {info.address.assign(v.via.str.ptr, v.via.str.size);}
      else if (key == "message_type") {info.message_type.assign(v.via.str.ptr, v.via.str.size);}
      else if (key == "fingerprint") {info.fingerprint = v.via.u64;}
      else if (key == "publisher_node") {info.publisher_node.assign(v.via.str.ptr, v.via.str.size);}
    }
    return info;
  }

  void send_status(DiscoveryStatus status, const std::string & msg)
  {
    msgpack::sbuffer buf;
    msgpack::packer<msgpack::sbuffer> pk(buf);
    pk.pack_map(2);
    pk.pack(std::string("status")); pk.pack(static_cast<std::int32_t>(status));
    pk.pack(std::string("message")); pk.pack(msg);
    zmq::message_t out(buf.data(), buf.size());
    (void)socket_.send(out, zmq::send_flags::none);
  }

  void send_topic_info(DiscoveryStatus status, const std::string & msg,
    const TopicInfo & info)
  {
    msgpack::sbuffer info_buf;
    msgpack::packer<msgpack::sbuffer> info_pk(info_buf);
    info_pk.pack_map(5);
    info_pk.pack(std::string("name")); info_pk.pack(info.name);
    info_pk.pack(std::string("address")); info_pk.pack(info.address);
    info_pk.pack(std::string("message_type")); info_pk.pack(info.message_type);
    info_pk.pack(std::string("fingerprint")); info_pk.pack(info.fingerprint);
    info_pk.pack(std::string("publisher_node")); info_pk.pack(info.publisher_node);

    msgpack::sbuffer buf;
    msgpack::packer<msgpack::sbuffer> pk(buf);
    pk.pack_map(3);
    pk.pack(std::string("status")); pk.pack(static_cast<std::int32_t>(status));
    pk.pack(std::string("message")); pk.pack(msg);
    pk.pack(std::string("topic_info"));
    pk.pack_bin(static_cast<std::uint32_t>(info_buf.size()));
    pk.pack_bin_body(info_buf.data(), info_buf.size());

    zmq::message_t out(buf.data(), buf.size());
    (void)socket_.send(out, zmq::send_flags::none);
  }

  zmq::socket_t socket_;
  std::atomic<bool> running_{false};
  std::thread thread_;
  std::mutex mu_;
  std::unordered_map<std::string, TopicInfo> topics_;
};

// Pump messages until the predicate is true or we hit a timeout.
template<typename Pred>
bool wait_for(Pred pred, std::chrono::milliseconds timeout = std::chrono::milliseconds(2000))
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) {return true;}
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return pred();
}

std::string unique_inproc(const std::string & prefix)
{
  static std::atomic<int> counter{0};
  return "inproc://" + prefix + "_" + std::to_string(counter.fetch_add(1));
}

std::string unique_ipc_dir()
{
  static std::atomic<int> counter{0};
  auto p = std::filesystem::temp_directory_path() /
    ("cortex_wire_test_" + std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(p);
  return "ipc://" + p.string() + "/";
}

}  // namespace

// Direct ctor — bypasses discovery, exercises just the recv path. We bind a
// PUB socket manually instead of going through Publisher so we can isolate
// the subscriber under test.
TEST(SubscriberDirect, ReceivesMultiPartFrameAndDecodesHeader)
{
  Context ctx;
  const auto endpoint = unique_inproc("subdirect");

  zmq::socket_t pub(ctx.raw(), zmq::socket_type::pub);
  pub.set(zmq::sockopt::linger, 0);
  pub.bind(endpoint);

  std::atomic<int> seen{0};
  std::atomic<std::uint64_t> last_seq{0};
  Subscriber sub(
    ctx, endpoint, "topic", /*fingerprint=*/0xabcdef0011223344ULL,
    [&](const Subscriber::Inbound & in) {
      last_seq.store(in.header.sequence, std::memory_order_relaxed);
      seen.fetch_add(1, std::memory_order_relaxed);
    });

  // Give the SUB time to subscribe before we publish. inproc transport
  // doesn't have late-joiner semantics; missed messages are dropped.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Pack a 1-field DictMessage-ish payload (a single str). The metadata
  // body shape doesn't matter for this test — we just verify the framing
  // and header reach the callback.
  MetadataBuilder b(1);
  b.pack_str("hello");
  auto frames = std::move(b).finish();

  // Build the wire header manually so we can verify the sequence the
  // subscriber sees.
  cortex_wire::MessageHeader hdr{0xabcdef0011223344ULL, 1'700'000'000ULL, 7ULL};
  std::array<std::uint8_t, cortex_wire::MessageHeader::kSize> hdr_bytes{};
  hdr.to_bytes(hdr_bytes.data());

  auto send_frame = [&](const void * data, std::size_t size, bool more) {
      zmq::message_t m(size);
      std::memcpy(m.data(), data, size);
      pub.send(m, more ? zmq::send_flags::sndmore : zmq::send_flags::none);
    };
  const std::string topic = "topic";
  send_frame(topic.data(), topic.size(), true);
  send_frame(hdr_bytes.data(), hdr_bytes.size(), true);
  send_frame(frames.metadata.data(), frames.metadata.size(), false);

  ASSERT_TRUE(wait_for([&]() {return seen.load() >= 1;}));
  EXPECT_EQ(last_seq.load(), 7u);
}

TEST(SubscriberDirect, DropsMessagesWithMismatchedFingerprint)
{
  Context ctx;
  const auto endpoint = unique_inproc("subfpmismatch");

  zmq::socket_t pub(ctx.raw(), zmq::socket_type::pub);
  pub.set(zmq::sockopt::linger, 0);
  pub.bind(endpoint);

  std::atomic<int> delivered{0};
  std::atomic<int> errors{0};
  Subscriber sub(
    ctx, endpoint, "topic", /*expected_fp=*/0xAAAAAAAAAAAAAAAAULL,
    [&](const Subscriber::Inbound &) {delivered.fetch_add(1);},
    [&](std::string_view) {errors.fetch_add(1);});

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  MetadataBuilder b(1);
  b.pack_int(42);
  auto frames = std::move(b).finish();

  // Wrong fingerprint on the wire.
  cortex_wire::MessageHeader hdr{0xBBBBBBBBBBBBBBBBULL, 0ULL, 0ULL};
  std::array<std::uint8_t, cortex_wire::MessageHeader::kSize> hdr_bytes{};
  hdr.to_bytes(hdr_bytes.data());

  auto send_frame = [&](const void * data, std::size_t size, bool more) {
      zmq::message_t m(size);
      std::memcpy(m.data(), data, size);
      pub.send(m, more ? zmq::send_flags::sndmore : zmq::send_flags::none);
    };
  const std::string topic = "topic";
  send_frame(topic.data(), topic.size(), true);
  send_frame(hdr_bytes.data(), hdr_bytes.size(), true);
  send_frame(frames.metadata.data(), frames.metadata.size(), false);

  // Wait a bit longer than the rcvtimeo so the wire pump has run.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  EXPECT_EQ(delivered.load(), 0);
  EXPECT_GE(errors.load(), 1);
}

// End-to-end via discovery — Publisher registers, Subscriber::connect()
// looks up, messages round-trip including the OOB-bearing case (one numpy-
// shaped buffer) and the inline-only case (primitive-style payload).
TEST(PubSubViaDiscovery, RoundTripsInlineAndOobMessages)
{
  Context ctx;
  const auto daemon_addr = unique_inproc("daemon");
  MockDaemon daemon(ctx.raw(), daemon_addr);

  DiscoveryClient discovery(ctx.raw(), daemon_addr);
  const std::uint64_t fp = 0xDECAFBADCAFEC0DEULL;

  // Use a dedicated ipc:// dir so the bound socket doesn't collide with the
  // host's real /tmp/cortex/topics state.
  const auto ipc_prefix = unique_ipc_dir();

  Publisher pub(
    ctx, discovery, /*topic=*/"unit_test_topic",
    /*cortex_type_name=*/"DictMessage", /*fingerprint=*/fp,
    /*publisher_node_id=*/"test_node",
    /*queue_size=*/100,
    ipc_prefix);

  std::mutex mu;
  std::condition_variable cv;
  std::vector<std::uint64_t> seqs;
  std::vector<std::size_t> oob_counts;

  auto sub = Subscriber::connect(
    ctx, discovery, "unit_test_topic", fp,
    [&](const Subscriber::Inbound & in) {
      std::lock_guard<std::mutex> lk(mu);
      seqs.push_back(in.header.sequence);
      oob_counts.push_back(in.oob_frames.size());
      cv.notify_all();
    });

  // SUB sockets need a beat to actually subscribe.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // --- Inline-only message (no OOB) ---
  {
    MetadataBuilder b(1);
    b.pack_str("inline_only");
    EXPECT_TRUE(pub.publish(std::move(b).finish()));
  }

  // --- OOB-bearing message (one numpy buffer) ---
  {
    MetadataBuilder b(1);
    const std::vector<float> arr{1.0F, 2.0F, 3.0F, 4.0F};
    b.pack_numpy_oob("<f4", {4}, arr.data(), arr.size() * sizeof(float));
    EXPECT_TRUE(pub.publish(std::move(b).finish()));
  }

  {
    std::unique_lock<std::mutex> lk(mu);
    EXPECT_TRUE(
      cv.wait_for(lk, std::chrono::seconds(2),
        [&] {return seqs.size() >= 2;}));
  }

  ASSERT_EQ(seqs.size(), 2u);
  EXPECT_EQ(seqs[0], 0u);
  EXPECT_EQ(seqs[1], 1u);
  EXPECT_EQ(oob_counts[0], 0u);                 // inline-only
  EXPECT_EQ(oob_counts[1], 1u);                 // one OOB frame

  // Cleanup: subscriber dtor joins thread; publisher dtor unregisters.
}

TEST(PubSubViaDiscovery, ConnectFailsLoudlyOnUnknownTopic)
{
  Context ctx;
  const auto daemon_addr = unique_inproc("daemon_unknown");
  MockDaemon daemon(ctx.raw(), daemon_addr);
  DiscoveryClient discovery(ctx.raw(), daemon_addr);

  EXPECT_THROW(
    Subscriber::connect(
      ctx, discovery, "nope", 1ULL,
      [](const Subscriber::Inbound &) {}),
    std::runtime_error);
}
