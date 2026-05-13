#ifndef CORTEX_WIRE__SUBSCRIBER_HPP_
#define CORTEX_WIRE__SUBSCRIBER_HPP_

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <zmq.hpp>

#include "cortex_wire/context.hpp"
#include "cortex_wire/discovery_client.hpp"
#include "cortex_wire/header.hpp"
#include "cortex_wire/metadata.hpp"
#include "cortex_wire/oob_buffer.hpp"

namespace cortex_wire
{

// Threaded SUB-side cortex client. Owns a ZMQ SUB socket and a recv thread
// that decodes incoming multipart messages (header + msgpack metadata + zero
// or more OOB frames) and delivers them to the user callback.
//
// Lifetime:
//   - ctor opens the SUB socket, subscribes to `topic`, spawns the recv
//     thread. The thread is running on return.
//   - dtor signals the thread to stop and joins it. Any in-flight callback
//     completes before the dtor returns.
//
// Threading: the user `on_message` callback is invoked on the recv thread,
// one message at a time. If the callback throws, the exception is caught
// and forwarded to `on_error` (which defaults to silent); the recv thread
// keeps running.
//
// Fingerprint policy: always strict. Messages whose wire header fingerprint
// differs from `expected_fingerprint` are dropped and reported via on_error.
//
// Inline-only payloads (no OOB descriptors at all) are handled transparently:
// the callback receives an empty `oob_frames` vector.
class Subscriber
{
public:
  struct Inbound
  {
    MessageHeader header;
    const DecodedMetadata & metadata;
    const std::vector<ZmqFramePtr> & oob_frames;
  };

  using MessageCallback = std::function<void (const Inbound &)>;
  using ErrorCallback = std::function<void (std::string_view what)>;

  // Direct constructor. Caller has already done the discovery lookup and
  // owns the endpoint + fingerprint contract.
  //
  // Throws std::runtime_error if connecting to `endpoint` fails. Otherwise
  // the recv thread is running on return.
  Subscriber(
    Context ctx,
    std::string endpoint,
    std::string topic,
    std::uint64_t expected_fingerprint,
    MessageCallback on_message,
    ErrorCallback on_error = {});

  // Convenience factory: look up the topic via the supplied DiscoveryClient
  // and delegate to the direct constructor. Throws if the topic isn't
  // registered or its registered fingerprint doesn't match.
  static Subscriber connect(
    Context ctx,
    DiscoveryClient & discovery,
    std::string topic,
    std::uint64_t expected_fingerprint,
    MessageCallback on_message,
    ErrorCallback on_error = {});

  ~Subscriber();

  Subscriber(const Subscriber &) = delete;
  Subscriber & operator=(const Subscriber &) = delete;
  // Non-movable: the recv thread captures `this` via the running_ flag and
  // socket reference. Wrap in std::unique_ptr if you need to move ownership.
  Subscriber(Subscriber &&) = delete;
  Subscriber & operator=(Subscriber &&) = delete;

  const std::string & topic() const noexcept {return topic_;}
  const std::string & endpoint() const noexcept {return endpoint_;}
  std::uint64_t expected_fingerprint() const noexcept {return expected_fp_;}

private:
  void recv_loop();
  void report_error(std::string what);

  Context ctx_;                  // shared ownership of the ZMQ context
  std::string endpoint_;
  std::string topic_;
  std::uint64_t expected_fp_;
  MessageCallback on_message_;
  ErrorCallback on_error_;

  zmq::socket_t sock_;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace cortex_wire

#endif  // CORTEX_WIRE__SUBSCRIBER_HPP_
