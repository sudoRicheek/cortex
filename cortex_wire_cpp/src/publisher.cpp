#include "cortex_wire/publisher.hpp"

#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "cortex_wire/header.hpp"

namespace cortex_wire
{

namespace
{

constexpr std::string_view kIpcPrefix = "ipc://";

}  // namespace

std::string Publisher::slugify(std::string s)
{
  for (auto & c : s) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') {
      c = '_';
    }
  }
  return s;
}

std::string Publisher::build_endpoint(
  const std::string & prefix,
  const std::string & node_id,
  const std::string & topic)
{
  // Mirrors the slugging convention the Python publisher and the
  // ros2 bridge already use: <prefix><node_slug>__<topic_slug>.sock when
  // prefix ends with a directory separator, else <prefix><node>__<topic>.
  std::string ep = prefix;
  if (!ep.empty() && ep.back() != '/' && ep.back() != '_') {
    ep.push_back('_');
  }
  ep += slugify(node_id);
  ep += "__";
  ep += slugify(topic);
  if (prefix.rfind(kIpcPrefix, 0) == 0) {
    ep += ".sock";
  }
  return ep;
}

void Publisher::ensure_parent_dir(const std::string & endpoint)
{
  if (endpoint.rfind(kIpcPrefix, 0) != 0) {
    return;                                     // tcp:// etc — no fs needed
  }
  std::filesystem::path p(endpoint.substr(kIpcPrefix.size()));
  std::error_code ec;
  std::filesystem::create_directories(p.parent_path(), ec);
  if (ec) {
    throw std::runtime_error(
            "cortex_wire::Publisher: cannot create parent dir for " +
            endpoint + ": " + ec.message());
  }
}

Publisher::Publisher(
  Context ctx,
  DiscoveryClient & discovery,
  std::string topic,
  std::string cortex_type_name,
  std::uint64_t fingerprint,
  std::string publisher_node_id,
  std::uint32_t queue_size,
  std::string endpoint_prefix)
: ctx_(std::move(ctx)),
  discovery_(&discovery),
  topic_(std::move(topic)),
  cortex_type_(std::move(cortex_type_name)),
  fingerprint_(fingerprint),
  node_id_(std::move(publisher_node_id)),
  sock_(ctx_.raw(), zmq::socket_type::pub)
{
  endpoint_ = build_endpoint(endpoint_prefix, node_id_, topic_);
  ensure_parent_dir(endpoint_);

  sock_.set(zmq::sockopt::linger, 0);
  sock_.set(zmq::sockopt::sndhwm, static_cast<int>(queue_size));
  try {
    sock_.bind(endpoint_);
  } catch (const zmq::error_t & e) {
    throw std::runtime_error(
            "cortex_wire::Publisher: bind(" + endpoint_ + ") failed: " +
            e.what());
  }

  TopicInfo info{topic_, endpoint_, cortex_type_, fingerprint_, node_id_};
  try {
    discovery_->register_topic(info);
    registered_ = true;
  } catch (const std::exception & e) {
    // The bind succeeded; close the socket on the way out.
    sock_.close();
    throw std::runtime_error(
            std::string("cortex_wire::Publisher: discovery register('") +
            topic_ + "') failed: " + e.what());
  }
}

Publisher::~Publisher()
{
  if (registered_ && discovery_) {
    try {
      discovery_->unregister_topic(topic_);
    } catch (...) {
      // Swallow — we are in a destructor; the daemon may also be down on
      // process exit. Leaking a stale entry is preferable to terminate().
    }
  }
}

bool Publisher::publish(MetadataBuilder::Frames frames)
{
  // Build the wire header.
  const std::uint64_t seq = sequence_.fetch_add(1, std::memory_order_relaxed);
  const std::uint64_t now_ns = static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  MessageHeader hdr{fingerprint_, now_ns, seq};

  std::array<std::uint8_t, MessageHeader::kSize> hdr_bytes{};
  hdr.to_bytes(hdr_bytes.data());

  // Multipart layout: [topic, header, metadata, *oob_buffers]
  const std::size_t frame_count = 3 + frames.oob_buffers.size();
  std::size_t i = 0;
  bool all_ok = true;

  auto send_one = [&](const void * data, std::size_t size) {
      zmq::message_t m(size);
      std::memcpy(m.data(), data, size);
      const auto flags = (i + 1 < frame_count) ?
        (zmq::send_flags::sndmore | zmq::send_flags::dontwait) :
        zmq::send_flags::dontwait;
      const auto r = sock_.send(m, flags);
      if (!r) {
        all_ok = false;
      }
      ++i;
    };

  send_one(topic_.data(), topic_.size());
  send_one(hdr_bytes.data(), hdr_bytes.size());
  send_one(frames.metadata.data(), frames.metadata.size());
  for (const auto & buf : frames.oob_buffers) {
    send_one(buf.data(), buf.size());
  }
  return all_ok;
}

}  // namespace cortex_wire
