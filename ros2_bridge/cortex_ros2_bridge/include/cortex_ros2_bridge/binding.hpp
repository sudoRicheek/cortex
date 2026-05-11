// Copyright (c) 2026, Cortex contributors. Apache-2.0.
#ifndef CORTEX_ROS2_BRIDGE__BINDING_HPP_
#define CORTEX_ROS2_BRIDGE__BINDING_HPP_

#include <cortex_wire/discovery_client.hpp>
#include <cortex_wire/header.hpp>
#include <cortex_wire/metadata.hpp>
#include <cortex_wire/oob_buffer.hpp>
#include <rclcpp/rclcpp.hpp>
#include <zmq.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "cortex_ros2_bridge/adapter.hpp"
#include "cortex_ros2_bridge/config.hpp"

namespace cortex_ros2_bridge
{

// Non-template binding interfaces used by the bridge components to manage
// the bridges uniformly. The component holds a vector of these.

class CortexToRos2BindingBase
{
public:
  virtual ~CortexToRos2BindingBase() = default;
  virtual void start() = 0;
  virtual void stop() = 0;
  virtual const std::string & entry_name() const = 0;
};

class Ros2ToCortexBindingBase
{
public:
  virtual ~Ros2ToCortexBindingBase() = default;
  // start() is a no-op for ros2_to_cortex (the rclcpp subscription is created
  // at construction time and starts receiving immediately when the executor
  // spins). It is kept symmetric with the other direction for the component's
  // benefit.
  virtual void start() = 0;
  virtual void stop() = 0;
  virtual const std::string & entry_name() const = 0;
};

// ---- CortexToRos2BindingImpl ---------------------------------------------

// One-per-bridge-entry: owns a SUB socket on its own thread, decodes incoming
// multipart frames, looks up the adapter, and publishes the resulting ROS 2
// message via a templated rclcpp::Publisher.
template<typename Ros2Msg>
class CortexToRos2BindingImpl : public CortexToRos2BindingBase
{
public:
  CortexToRos2BindingImpl(
    rclcpp::Node * node,
    zmq::context_t * ctx,
    BridgeEntry cfg,
    cortex_wire::TopicInfo topic_info,
    std::shared_ptr<CortexToRos2Adapter<Ros2Msg>> adapter,
    const rclcpp::QoS & qos)
  : node_(node),
    ctx_(ctx),
    cfg_(std::move(cfg)),
    topic_info_(std::move(topic_info)),
    adapter_(std::move(adapter)),
    publisher_(node_->create_publisher<Ros2Msg>(cfg_.ros2.topic, qos))
  {
  }

  ~CortexToRos2BindingImpl() override
  {
    stop();
  }

  void start() override
  {
    if (thread_.joinable()) {return;}
    running_.store(true);
    thread_ = std::thread(&CortexToRos2BindingImpl::recv_loop, this);
  }

  void stop() override
  {
    if (!running_.exchange(false)) {return;}
    if (thread_.joinable()) {thread_.join();}
  }

  const std::string & entry_name() const override {return cfg_.name;}

private:
  void recv_loop()
  {
    zmq::socket_t sub(*ctx_, zmq::socket_type::sub);
    sub.set(zmq::sockopt::linger, 0);
    // 100 ms recv timeout so stop() makes us notice promptly.
    sub.set(zmq::sockopt::rcvtimeo, 100);
    try {
      sub.connect(topic_info_.address);
    } catch (const zmq::error_t & e) {
      RCLCPP_ERROR(
        node_->get_logger(), "[%s] connect(%s) failed: %s",
        cfg_.name.c_str(), topic_info_.address.c_str(), e.what());
      return;
    }
    sub.set(zmq::sockopt::subscribe, cfg_.cortex.topic);

    std::vector<zmq::message_t> frames;
    while (running_.load()) {
      frames.clear();
      bool got_one = false;
      // Read a full multipart message.
      while (true) {
        zmq::message_t f;
        zmq::recv_result_t r;
        try {
          r = sub.recv(f, zmq::recv_flags::none);
        } catch (const zmq::error_t & e) {
          if (e.num() == ETERM) {return;}
          RCLCPP_WARN(node_->get_logger(), "[%s] recv error: %s", cfg_.name.c_str(), e.what());
          break;
        }
        if (!r) {break;}  // timeout
        const bool has_more = f.more();
        frames.emplace_back(std::move(f));
        got_one = true;
        if (!has_more) {break;}
      }
      if (!got_one) {continue;}
      if (frames.size() < 3) {
        RCLCPP_WARN(
          node_->get_logger(), "[%s] short message: %zu frames", cfg_.name.c_str(),
          frames.size());
        continue;
      }

      try {
        const auto header = cortex_wire::MessageHeader::from_bytes(
          frames[1].data(), frames[1].size());
        if (header.fingerprint != topic_info_.fingerprint) {
          RCLCPP_WARN(
            node_->get_logger(),
            "[%s] fingerprint mismatch: header=0x%016lx expected=0x%016lx — dropping",
            cfg_.name.c_str(),
            static_cast<unsigned long>(header.fingerprint),
            static_cast<unsigned long>(topic_info_.fingerprint));
          continue;
        }
        const auto metadata = cortex_wire::DecodedMetadata::from_bytes(
          frames[2].data(), frames[2].size());

        std::vector<cortex_wire::ZmqFramePtr> oob;
        oob.reserve(frames.size() > 3 ? frames.size() - 3 : 0);
        for (std::size_t i = 3; i < frames.size(); ++i) {
          oob.push_back(cortex_wire::make_owned(std::move(frames[i])));
        }

        const CortexInbound in{header, metadata, oob, cfg_};
        auto msg = adapter_->to_ros2(in);
        if (msg) {
          publisher_->publish(std::move(msg));
        }
      } catch (const std::exception & e) {
        RCLCPP_WARN(node_->get_logger(), "[%s] decode error: %s", cfg_.name.c_str(), e.what());
      }
    }
  }

  rclcpp::Node * node_;
  zmq::context_t * ctx_;
  BridgeEntry cfg_;
  cortex_wire::TopicInfo topic_info_;
  std::shared_ptr<CortexToRos2Adapter<Ros2Msg>> adapter_;
  typename rclcpp::Publisher<Ros2Msg>::SharedPtr publisher_;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

// ---- Ros2ToCortexBindingImpl ---------------------------------------------

// One-per-bridge-entry: subscribes via rclcpp, packs Cortex frames in the
// callback, and emits over a single PUB socket. The rclcpp executor
// guarantees serialised callbacks per subscription (single-threaded callback
// group by default), so the PUB socket is only touched from one thread.
template<typename Ros2Msg>
class Ros2ToCortexBindingImpl : public Ros2ToCortexBindingBase
{
public:
  Ros2ToCortexBindingImpl(
    rclcpp::Node * node,
    zmq::context_t * ctx,
    BridgeEntry cfg,
    std::shared_ptr<Ros2ToCortexAdapter<Ros2Msg>> adapter,
    std::string pub_endpoint,
    std::uint64_t fingerprint,
    const rclcpp::QoS & qos)
  : node_(node),
    ctx_(ctx),
    cfg_(std::move(cfg)),
    adapter_(std::move(adapter)),
    pub_endpoint_(std::move(pub_endpoint)),
    fingerprint_(fingerprint),
    pub_socket_(*ctx_, zmq::socket_type::pub)
  {
    pub_socket_.set(zmq::sockopt::linger, 0);
    pub_socket_.bind(pub_endpoint_);

    subscription_ = node_->create_subscription<Ros2Msg>(
      cfg_.ros2.topic, qos,
      [this](std::shared_ptr<const Ros2Msg> msg) {on_ros2(*msg);});
  }

  ~Ros2ToCortexBindingImpl() override
  {
    stop();
  }

  void start() override {}
  void stop() override
  {
    // Drop the subscription first — rclcpp guarantees no further callbacks
    // touch this object after the SharedPtr is reset.
    subscription_.reset();
  }

  const std::string & entry_name() const override {return cfg_.name;}
  const std::string & pub_endpoint() const {return pub_endpoint_;}

private:
  void on_ros2(const Ros2Msg & msg)
  {
    try {
      const auto seq = sequence_.fetch_add(1);
      auto out = adapter_->to_cortex(msg, seq, cfg_);

      const std::uint64_t now_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
      cortex_wire::MessageHeader header{fingerprint_, now_ns, seq};

      std::array<std::uint8_t, cortex_wire::MessageHeader::kSize> header_bytes{};
      header.to_bytes(header_bytes.data());

      // Multipart: [topic, header, metadata, *oob]
      const std::size_t frame_count = 3 + out.oob_buffers.size();
      std::size_t i = 0;

      auto send = [&](const void * data, std::size_t size) {
          zmq::message_t m(size);
          std::memcpy(m.data(), data, size);
          const auto flags = (i + 1 < frame_count) ?
            zmq::send_flags::sndmore : zmq::send_flags::none;
          (void)pub_socket_.send(m, flags);
          ++i;
        };

      send(cfg_.cortex.topic.data(), cfg_.cortex.topic.size());
      send(header_bytes.data(), header_bytes.size());
      send(out.metadata.data(), out.metadata.size());
      for (const auto & buf : out.oob_buffers) {
        send(buf.data(), buf.size());
      }
    } catch (const std::exception & e) {
      RCLCPP_WARN(node_->get_logger(), "[%s] publish error: %s", cfg_.name.c_str(), e.what());
    }
  }

  rclcpp::Node * node_;
  zmq::context_t * ctx_;
  BridgeEntry cfg_;
  std::shared_ptr<Ros2ToCortexAdapter<Ros2Msg>> adapter_;
  std::string pub_endpoint_;
  std::uint64_t fingerprint_;
  zmq::socket_t pub_socket_;
  typename rclcpp::Subscription<Ros2Msg>::SharedPtr subscription_;
  std::atomic<std::uint64_t> sequence_{0};
};

}  // namespace cortex_ros2_bridge

#endif  // CORTEX_ROS2_BRIDGE__BINDING_HPP_
