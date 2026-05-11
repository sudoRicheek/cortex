// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#ifndef CORTEX_ROS2_BRIDGE__CORTEX_WIRE__DISCOVERY_CLIENT_HPP_
#define CORTEX_ROS2_BRIDGE__CORTEX_WIRE__DISCOVERY_CLIENT_HPP_

#include <zmq.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace cortex_ros2_bridge::cortex_wire
{

// Mirrors cortex.discovery.protocol.DiscoveryCommand.
enum class DiscoveryCommand : std::int32_t
{
  RegisterTopic = 1,
  UnregisterTopic = 2,
  LookupTopic = 3,
  ListTopics = 4,
  Ping = 5,
  Shutdown = 99,
};

// Mirrors cortex.discovery.protocol.DiscoveryStatus.
enum class DiscoveryStatus : std::int32_t
{
  Ok = 0,
  NotFound = 1,
  AlreadyExists = 2,
  Error = 3,
};

struct TopicInfo
{
  std::string name;
  std::string address;        // ZMQ endpoint, e.g. "ipc:///tmp/cortex/topics/foo"
  std::string message_type;
  std::uint64_t fingerprint = 0;
  std::string publisher_node;
};

class DiscoveryError : public std::runtime_error
{
public:
  using std::runtime_error::runtime_error;
};

// Minimal sync REQ/REP client for the Cortex discovery daemon. Each method
// is one round-trip with a configurable timeout; sockets are recreated on
// timeout (REQ/REP gets stuck after a missed reply, so we recycle the socket
// to recover).
class DiscoveryClient
{
public:
  DiscoveryClient(
    zmq::context_t & context, std::string address,
    std::chrono::milliseconds request_timeout = std::chrono::milliseconds(1000));

  // Resolve a topic to its publisher endpoint.
  // Returns nullopt on NOT_FOUND, throws DiscoveryError on transport/parse
  // errors or non-OK / non-NOT_FOUND status codes.
  std::optional<TopicInfo> lookup(const std::string & topic_name);

  // Register a publisher with the daemon. Throws on any non-OK status.
  void register_topic(const TopicInfo & info);

  // Unregister by name. Tolerates NOT_FOUND.
  void unregister_topic(const std::string & topic_name);

  // ---- low-level encode/decode helpers, exposed for unit tests ----
  static std::vector<std::uint8_t> encode_request(
    DiscoveryCommand cmd, const std::optional<std::string> & topic_name,
    const std::optional<TopicInfo> & topic_info);

  struct DecodedResponse
  {
    DiscoveryStatus status;
    std::string message;
    std::optional<TopicInfo> topic_info;
    std::vector<TopicInfo> topics;
  };

  static DecodedResponse decode_response(const void * data, std::size_t size);

private:
  std::vector<std::uint8_t> request_blocking(const std::vector<std::uint8_t> & req);
  void reset_socket();

  zmq::context_t & ctx_;
  std::string address_;
  std::chrono::milliseconds timeout_;
  zmq::socket_t socket_;
};

}  // namespace cortex_ros2_bridge::cortex_wire

#endif  // CORTEX_ROS2_BRIDGE__CORTEX_WIRE__DISCOVERY_CLIENT_HPP_
