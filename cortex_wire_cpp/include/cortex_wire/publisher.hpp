#ifndef CORTEX_WIRE__PUBLISHER_HPP_
#define CORTEX_WIRE__PUBLISHER_HPP_

#include <atomic>
#include <cstdint>
#include <string>

#include <zmq.hpp>

#include "cortex_wire/context.hpp"
#include "cortex_wire/discovery_client.hpp"
#include "cortex_wire/metadata.hpp"

namespace cortex_wire
{

// PUB-side cortex client. Single-producer (ZMQ PUB is inherently
// single-producer; callers must serialize publish() calls).
//
// Lifetime:
//   - ctor builds a deterministic ipc:// endpoint under `endpoint_prefix`,
//     creates the parent directory if missing, binds a PUB socket, and
//     registers the topic with the discovery daemon.
//   - dtor unregisters the topic and closes the socket.
//
// Each publish() injects a fresh header (fp, monotonic seq, timestamp_ns)
// and emits a multipart message: [topic, header, metadata, *oob_buffers].
// Inline-only messages (oob_buffers empty) send 3 frames; OOB-bearing
// messages send 3 + N.
class Publisher
{
public:
  // Throws std::runtime_error on bind / mkdir / discovery-register failure.
  Publisher(
    Context ctx,
    DiscoveryClient & discovery,
    std::string topic,
    std::string cortex_type_name,
    std::uint64_t fingerprint,
    std::string publisher_node_id,
    std::uint32_t queue_size = 1000,
    std::string endpoint_prefix = "ipc:///tmp/cortex/topics/");

  ~Publisher();

  Publisher(const Publisher &) = delete;
  Publisher & operator=(const Publisher &) = delete;
  Publisher(Publisher &&) = delete;
  Publisher & operator=(Publisher &&) = delete;

  // Send one message. The caller is responsible for serialising calls.
  //
  // Returns true on send, false if ZMQ reported the message was dropped
  // (e.g. queue overflow under NOBLOCK semantics, which is the default for
  // PUB sockets — same behaviour as the Python client). The sequence
  // counter advances on every call regardless of return value, mirroring
  // Python.
  bool publish(MetadataBuilder::Frames frames);

  const std::string & topic() const noexcept {return topic_;}
  const std::string & endpoint() const noexcept {return endpoint_;}
  const std::string & cortex_type_name() const noexcept {return cortex_type_;}
  std::uint64_t fingerprint() const noexcept {return fingerprint_;}
  std::uint64_t sequence() const noexcept
  {
    return sequence_.load(std::memory_order_relaxed);
  }

private:
  static std::string slugify(std::string s);
  static std::string build_endpoint(
    const std::string & prefix,
    const std::string & node_id,
    const std::string & topic);
  static void ensure_parent_dir(const std::string & endpoint);

  Context ctx_;                 // shared ownership of the ZMQ context
  DiscoveryClient * discovery_; // non-owning; caller outlives Publisher
  std::string topic_;
  std::string endpoint_;
  std::string cortex_type_;
  std::uint64_t fingerprint_;
  std::string node_id_;

  zmq::socket_t sock_;
  std::atomic<std::uint64_t> sequence_{0};
  bool registered_ = false;
};

}  // namespace cortex_wire

#endif  // CORTEX_WIRE__PUBLISHER_HPP_
