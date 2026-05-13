#include "cortex_wire/subscriber.hpp"

#include <stdexcept>
#include <utility>

namespace cortex_wire
{

namespace
{

// 100 ms wakeup gives the dtor a bounded time to stop the recv thread.
constexpr int kRecvTimeoutMs = 100;

}  // namespace

Subscriber::Subscriber(
  Context ctx,
  std::string endpoint,
  std::string topic,
  std::uint64_t expected_fingerprint,
  MessageCallback on_message,
  ErrorCallback on_error)
: ctx_(std::move(ctx)),
  endpoint_(std::move(endpoint)),
  topic_(std::move(topic)),
  expected_fp_(expected_fingerprint),
  on_message_(std::move(on_message)),
  on_error_(std::move(on_error)),
  sock_(ctx_.raw(), zmq::socket_type::sub)
{
  if (!on_message_) {
    throw std::invalid_argument("cortex_wire::Subscriber: on_message is empty");
  }
  sock_.set(zmq::sockopt::linger, 0);
  sock_.set(zmq::sockopt::rcvtimeo, kRecvTimeoutMs);
  try {
    sock_.connect(endpoint_);
  } catch (const zmq::error_t & e) {
    throw std::runtime_error(
            "cortex_wire::Subscriber: connect(" + endpoint_ + ") failed: " +
            e.what());
  }
  sock_.set(zmq::sockopt::subscribe, topic_);

  running_.store(true);
  thread_ = std::thread(&Subscriber::recv_loop, this);
}

Subscriber Subscriber::connect(
  Context ctx,
  DiscoveryClient & discovery,
  std::string topic,
  std::uint64_t expected_fingerprint,
  MessageCallback on_message,
  ErrorCallback on_error)
{
  auto info = discovery.lookup(topic);
  if (!info) {
    throw std::runtime_error(
            "cortex_wire::Subscriber::connect: topic '" + topic +
            "' is not registered with discovery");
  }
  if (info->fingerprint != expected_fingerprint) {
    throw std::runtime_error(
            "cortex_wire::Subscriber::connect: fingerprint mismatch for topic '" +
            topic + "' (daemon reports a different message type than expected)");
  }
  return Subscriber(
    std::move(ctx),
    std::move(info->address),
    std::move(topic),
    expected_fingerprint,
    std::move(on_message),
    std::move(on_error));
}

Subscriber::~Subscriber()
{
  if (running_.exchange(false)) {
    if (thread_.joinable()) {
      thread_.join();
    }
  }
}

void Subscriber::report_error(std::string what)
{
  if (on_error_) {
    on_error_(what);
  }
}

void Subscriber::recv_loop()
{
  std::vector<zmq::message_t> frames;
  while (running_.load(std::memory_order_relaxed)) {
    frames.clear();

    // Pull a full multipart message. First recv may time out (so we can
    // notice running_); subsequent ones use the same timeout, which is fine
    // because frames within a single multipart arrive together.
    bool got_one = false;
    while (true) {
      zmq::message_t f;
      zmq::recv_result_t r;
      try {
        r = sock_.recv(f, zmq::recv_flags::none);
      } catch (const zmq::error_t & e) {
        if (e.num() == ETERM) {return;}
        report_error(std::string("recv: ") + e.what());
        break;
      }
      if (!r) {break;}                          // timeout, retry outer loop
      const bool has_more = f.more();
      frames.emplace_back(std::move(f));
      got_one = true;
      if (!has_more) {break;}
    }
    if (!got_one) {continue;}

    // [topic, header, metadata, *oob]
    if (frames.size() < 3) {
      report_error("short message: <3 frames");
      continue;
    }

    try {
      const auto header = MessageHeader::from_bytes(
        frames[1].data(), frames[1].size());
      if (header.fingerprint != expected_fp_) {
        report_error("fingerprint mismatch on wire — dropping");
        continue;
      }
      const auto metadata = DecodedMetadata::from_bytes(
        frames[2].data(), frames[2].size());

      std::vector<ZmqFramePtr> oob;
      oob.reserve(frames.size() > 3 ? frames.size() - 3 : 0);
      for (std::size_t i = 3; i < frames.size(); ++i) {
        oob.push_back(make_owned(std::move(frames[i])));
      }

      const Inbound in{header, metadata, oob};
      try {
        on_message_(in);
      } catch (const std::exception & e) {
        report_error(std::string("callback: ") + e.what());
      } catch (...) {
        report_error("callback: unknown exception");
      }
    } catch (const std::exception & e) {
      report_error(std::string("decode: ") + e.what());
    }
  }
}

}  // namespace cortex_wire
