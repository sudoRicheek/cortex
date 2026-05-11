// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#include "cortex_wire/discovery_client.hpp"

#include <msgpack.hpp>

#include <cstring>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace cortex_wire
{

namespace
{

std::string_view str_view(const msgpack::object & o)
{
  return std::string_view(o.via.str.ptr, o.via.str.size);
}

std::vector<std::uint8_t> pack_topic_info(const TopicInfo & info)
{
  msgpack::sbuffer buf;
  msgpack::packer<msgpack::sbuffer> pk(buf);
  pk.pack_map(5);
  pk.pack(std::string("name"));           pk.pack(info.name);
  pk.pack(std::string("address"));        pk.pack(info.address);
  pk.pack(std::string("message_type"));   pk.pack(info.message_type);
  pk.pack(std::string("fingerprint"));    pk.pack(info.fingerprint);
  pk.pack(std::string("publisher_node")); pk.pack(info.publisher_node);
  return std::vector<std::uint8_t>(
    reinterpret_cast<const std::uint8_t *>(buf.data()),
    reinterpret_cast<const std::uint8_t *>(buf.data()) + buf.size());
}

TopicInfo unpack_topic_info(const void * data, std::size_t size)
{
  msgpack::object_handle oh;
  try {
    oh = msgpack::unpack(static_cast<const char *>(data), size);
  } catch (const std::exception & e) {
    throw DiscoveryError(std::string("TopicInfo: msgpack error: ") + e.what());
  }
  const msgpack::object & root = oh.get();
  if (root.type != msgpack::type::MAP) {
    throw DiscoveryError("TopicInfo: expected msgpack map");
  }
  TopicInfo info;
  for (std::uint32_t i = 0; i < root.via.map.size; ++i) {
    const auto & k = root.via.map.ptr[i].key;
    const auto & v = root.via.map.ptr[i].val;
    if (k.type != msgpack::type::STR) {
      continue;
    }
    const auto key = str_view(k);
    if (key == "name" && v.type == msgpack::type::STR) {
      info.name = std::string(str_view(v));
    } else if (key == "address" && v.type == msgpack::type::STR) {
      info.address = std::string(str_view(v));
    } else if (key == "message_type" && v.type == msgpack::type::STR) {
      info.message_type = std::string(str_view(v));
    } else if (key == "fingerprint" && v.type == msgpack::type::POSITIVE_INTEGER) {
      info.fingerprint = v.via.u64;
    } else if (key == "publisher_node" && v.type == msgpack::type::STR) {
      info.publisher_node = std::string(str_view(v));
    }
  }
  return info;
}

}  // namespace

std::vector<std::uint8_t> DiscoveryClient::encode_request(
  DiscoveryCommand cmd, const std::optional<std::string> & topic_name,
  const std::optional<TopicInfo> & topic_info)
{
  msgpack::sbuffer buf;
  msgpack::packer<msgpack::sbuffer> pk(buf);

  // Cortex always packs "command" and "topic_name"; "topic_info" is included
  // only when non-null. Match that layout byte-for-byte so the daemon's
  // existing decoder accepts our requests.
  const std::size_t map_size = 2 + (topic_info.has_value() ? 1 : 0);
  pk.pack_map(static_cast<std::uint32_t>(map_size));

  pk.pack(std::string("command"));
  pk.pack(static_cast<std::int32_t>(cmd));

  pk.pack(std::string("topic_name"));
  if (topic_name) {
    pk.pack(*topic_name);
  } else {
    pk.pack_nil();
  }

  if (topic_info) {
    pk.pack(std::string("topic_info"));
    // Match the Python protocol: topic_info is packed *as bytes*, where the
    // bytes themselves are a msgpack-encoded map.
    const auto info_bytes = pack_topic_info(*topic_info);
    pk.pack_bin(static_cast<std::uint32_t>(info_bytes.size()));
    pk.pack_bin_body(
      reinterpret_cast<const char *>(info_bytes.data()), info_bytes.size());
  }

  return std::vector<std::uint8_t>(
    reinterpret_cast<const std::uint8_t *>(buf.data()),
    reinterpret_cast<const std::uint8_t *>(buf.data()) + buf.size());
}

DiscoveryClient::DecodedResponse DiscoveryClient::decode_response(
  const void * data, std::size_t size)
{
  DecodedResponse out;
  msgpack::object_handle oh;
  try {
    oh = msgpack::unpack(static_cast<const char *>(data), size);
  } catch (const std::exception & e) {
    throw DiscoveryError(std::string("response: msgpack error: ") + e.what());
  }
  const msgpack::object & root = oh.get();
  if (root.type != msgpack::type::MAP) {
    throw DiscoveryError("response: expected msgpack map");
  }

  bool status_set = false;
  out.status = DiscoveryStatus::Error;

  for (std::uint32_t i = 0; i < root.via.map.size; ++i) {
    const auto & k = root.via.map.ptr[i].key;
    const auto & v = root.via.map.ptr[i].val;
    if (k.type != msgpack::type::STR) {
      continue;
    }
    const auto key = str_view(k);
    if (key == "status") {
      if (v.type != msgpack::type::POSITIVE_INTEGER &&
        v.type != msgpack::type::NEGATIVE_INTEGER)
      {
        throw DiscoveryError("response: 'status' must be an integer");
      }
      out.status = static_cast<DiscoveryStatus>(
        v.type == msgpack::type::POSITIVE_INTEGER ? static_cast<std::int64_t>(v.via.u64) :
        v.via.i64);
      status_set = true;
    } else if (key == "message" && v.type == msgpack::type::STR) {
      out.message = std::string(str_view(v));
    } else if (key == "topic_info" && v.type == msgpack::type::BIN) {
      out.topic_info = unpack_topic_info(v.via.bin.ptr, v.via.bin.size);
    } else if (key == "topics" && v.type == msgpack::type::ARRAY) {
      out.topics.reserve(v.via.array.size);
      for (std::uint32_t j = 0; j < v.via.array.size; ++j) {
        const auto & item = v.via.array.ptr[j];
        if (item.type != msgpack::type::BIN) {
          throw DiscoveryError("response: 'topics' entry must be msgpack bin");
        }
        out.topics.push_back(unpack_topic_info(item.via.bin.ptr, item.via.bin.size));
      }
    }
  }
  if (!status_set) {
    throw DiscoveryError("response: missing 'status' field");
  }
  return out;
}

DiscoveryClient::DiscoveryClient(
  zmq::context_t & context, std::string address, std::chrono::milliseconds request_timeout)
: ctx_(context), address_(std::move(address)), timeout_(request_timeout),
  socket_(ctx_, zmq::socket_type::req)
{
  socket_.set(zmq::sockopt::linger, 0);
  socket_.set(
    zmq::sockopt::rcvtimeo, static_cast<int>(timeout_.count()));
  socket_.set(
    zmq::sockopt::sndtimeo, static_cast<int>(timeout_.count()));
  socket_.connect(address_);
}

void DiscoveryClient::reset_socket()
{
  socket_.close();
  socket_ = zmq::socket_t(ctx_, zmq::socket_type::req);
  socket_.set(zmq::sockopt::linger, 0);
  socket_.set(
    zmq::sockopt::rcvtimeo, static_cast<int>(timeout_.count()));
  socket_.set(
    zmq::sockopt::sndtimeo, static_cast<int>(timeout_.count()));
  socket_.connect(address_);
}

std::vector<std::uint8_t> DiscoveryClient::request_blocking(
  const std::vector<std::uint8_t> & req)
{
  zmq::message_t msg(req.data(), req.size());
  zmq::send_result_t sent;
  try {
    sent = socket_.send(msg, zmq::send_flags::none);
  } catch (const zmq::error_t & e) {
    reset_socket();
    throw DiscoveryError(std::string("send failed: ") + e.what());
  }
  if (!sent) {
    reset_socket();
    throw DiscoveryError("send timed out after " + std::to_string(timeout_.count()) + "ms");
  }

  zmq::message_t reply;
  zmq::recv_result_t got;
  try {
    got = socket_.recv(reply, zmq::recv_flags::none);
  } catch (const zmq::error_t & e) {
    reset_socket();
    throw DiscoveryError(std::string("recv failed: ") + e.what());
  }
  if (!got) {
    reset_socket();
    throw DiscoveryError("recv timed out after " + std::to_string(timeout_.count()) + "ms");
  }
  return std::vector<std::uint8_t>(
    static_cast<std::uint8_t *>(reply.data()),
    static_cast<std::uint8_t *>(reply.data()) + reply.size());
}

std::optional<TopicInfo> DiscoveryClient::lookup(const std::string & topic_name)
{
  const auto req = encode_request(DiscoveryCommand::LookupTopic, topic_name, std::nullopt);
  const auto reply = request_blocking(req);
  const auto resp = decode_response(reply.data(), reply.size());
  if (resp.status == DiscoveryStatus::NotFound) {
    return std::nullopt;
  }
  if (resp.status != DiscoveryStatus::Ok) {
    throw DiscoveryError(
      "lookup('" + topic_name + "') failed: " + resp.message +
      " (status=" + std::to_string(static_cast<int>(resp.status)) + ")");
  }
  if (!resp.topic_info) {
    throw DiscoveryError("lookup returned OK with no topic_info");
  }
  return resp.topic_info;
}

void DiscoveryClient::register_topic(const TopicInfo & info)
{
  const auto req = encode_request(DiscoveryCommand::RegisterTopic, std::nullopt, info);
  const auto reply = request_blocking(req);
  const auto resp = decode_response(reply.data(), reply.size());
  if (resp.status != DiscoveryStatus::Ok) {
    throw DiscoveryError(
      "register('" + info.name + "') failed: " + resp.message +
      " (status=" + std::to_string(static_cast<int>(resp.status)) + ")");
  }
}

void DiscoveryClient::unregister_topic(const std::string & topic_name)
{
  const auto req = encode_request(DiscoveryCommand::UnregisterTopic, topic_name, std::nullopt);
  const auto reply = request_blocking(req);
  const auto resp = decode_response(reply.data(), reply.size());
  if (resp.status != DiscoveryStatus::Ok && resp.status != DiscoveryStatus::NotFound) {
    throw DiscoveryError(
      "unregister('" + topic_name + "') failed: " + resp.message +
      " (status=" + std::to_string(static_cast<int>(resp.status)) + ")");
  }
}

}  // namespace cortex_wire
