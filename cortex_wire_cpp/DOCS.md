# cortex_wire_cpp — reference docs

C++ implementation of the Cortex IPC stack: wire codec, discovery client, threaded pub/sub. Standalone pure-CMake library, no ROS 2 or Python dependency.

This document defines what's in the library, what's missing on purpose, and how the surface compares to the Python reference.

## Scope

A **deliberate subset** of `cortex/core/` for plain-C++ consumers: the ROS 2 bridge, debug tools, sub-100 µs control loops, alternative-language bridges. No executor, timer, or async-event-loop model — C++ users bring their own runtime (rclcpp, `std::thread`, asio).

| Layer        | Components                                                                                |
|--------------|-------------------------------------------------------------------------------------------|
| Codec        | `MessageHeader`, `DecodedMetadata`, `MetadataBuilder`, `OobBuffer<T>`, `ZmqAllocator<T>`  |
| Discovery    | `DiscoveryClient` (REQ/REP, sync)                                                         |
| Pub/Sub      | `Context`, `Publisher`, `Subscriber`                                                      |
| Type catalog | `fingerprint_table` (generated from Python `cortex.messages.standard`)                    |

## Wire codec

Cortex multipart layout:

```
frame 0:    topic            (bytes)
frame 1:    MessageHeader    (24 bytes, big-endian: fp | ts_ns | seq)
frame 2:    metadata         (msgpack array of dataclass field values)
frame 3..N: OOB buffers      (raw numpy / torch payloads, one per OOB descriptor
                              referenced inside the metadata)
```

**`MessageHeader`** ([header.hpp](include/cortex_wire/header.hpp)) — 24-byte fixed header. `from_bytes` / `to_bytes`. Throws `WireDecodeError` on short input.

**`DecodedMetadata`** ([metadata.hpp](include/cortex_wire/metadata.hpp)) — owns a `msgpack::object_handle` tree. `field(i)` for random access; `as_oob(obj)` recognises `{"__cortex_oob__": ...}` descriptor maps. `walk_oob(obj, visitor)` recurses into nested dicts/lists — for adapters that need every OOB reachable from a top-level field (e.g. DictMessage with nested arrays).

**`MetadataBuilder`** ([metadata.hpp](include/cortex_wire/metadata.hpp)) — encoder. `pack_*` for primitives; `pack_numpy_oob` / `pack_torch_oob` write the descriptor inline and queue the raw buffer for emission as the next OOB frame. `finish()` returns `{metadata, oob_buffers}`.

**`OobBuffer<T>`** ([oob_buffer.hpp](include/cortex_wire/oob_buffer.hpp)) — typed read-only view into a ZMQ frame. Holds a `shared_ptr<zmq::message_t>` so the bytes outlive any caller holding the view. `data()`, `size()`, iterators.

**`ZmqAllocator<T>`** ([oob_buffer.hpp](include/cortex_wire/oob_buffer.hpp)) — stateful std-allocator that backs a `std::vector<T, ZmqAllocator<T>>` with an existing ZMQ frame. Useful only when you control the vector's allocator type; see the header comment for the value-init caveat. **Not usable** with stock `sensor_msgs/Image::data` (default allocator hard-coded).

**`fingerprint_table`** ([fingerprint_table.hpp](include/cortex_wire/fingerprint_table.hpp)) — generated lookup over `cortex.messages.standard`. `find_by_name("DictMessage")` returns `{kind, fingerprint}`. Regenerate with [scripts/gen_fingerprint_table.py](scripts/gen_fingerprint_table.py).

## Discovery client

[`DiscoveryClient`](include/cortex_wire/discovery_client.hpp) — sync REQ/REP client for `cortex-discovery`. One round-trip per call; sockets are recycled on REQ timeout to recover from the stuck-after-missed-reply state.

- `lookup(topic_name)` — `std::optional<TopicInfo>` (nullopt on NOT_FOUND).
- `register_topic(info)` — used by publishers in their ctor.
- `unregister_topic(name)` — tolerates NOT_FOUND; used in publisher dtors.

`encode_request` / `decode_response` are exposed for tests.

## Pub/Sub client

Three classes: [`Context`](include/cortex_wire/context.hpp), [`Publisher`](include/cortex_wire/publisher.hpp), [`Subscriber`](include/cortex_wire/subscriber.hpp).

### `Context`

```cpp
class Context {
 public:
  Context();                                  // owns a fresh zmq::context_t(1)
  explicit Context(zmq::context_t& external); // wrap externally-owned; no cleanup
  zmq::context_t& raw() noexcept;
};
```

Copyable, refcounted shared ownership. Last destruction runs `shutdown()` + `close()` to unblock any in-flight `recv()`.

### `Subscriber`

```cpp
class Subscriber {
 public:
  struct Inbound {
    MessageHeader header;
    const DecodedMetadata& metadata;
    const std::vector<ZmqFramePtr>& oob_frames;   // possibly empty
  };
  using MessageCallback = std::function<void(const Inbound&)>;
  using ErrorCallback   = std::function<void(std::string_view what)>;

  // Direct ctor — caller has done discovery lookup or knows the endpoint.
  Subscriber(Context ctx, std::string endpoint, std::string topic,
             std::uint64_t expected_fingerprint,
             MessageCallback on_message, ErrorCallback on_error = {});

  // Factory — does the discovery lookup; throws on not-found or fp mismatch.
  static Subscriber connect(Context ctx, DiscoveryClient& discovery,
                            std::string topic, std::uint64_t expected_fingerprint,
                            MessageCallback on_message, ErrorCallback on_error = {});

  ~Subscriber();   // joins recv thread (≤ 100 ms)
  // non-copy, non-move (thread-owning; wrap in unique_ptr to relocate)
};
```

- The ctor spawns a dedicated recv thread; it is running on return.
- **Always strict fingerprint.** Messages whose wire header fingerprint differs from `expected_fingerprint` are dropped and reported via `on_error`. Python's lax mode is intentionally omitted.
- The callback runs on the recv thread, one message at a time. Exceptions are caught and forwarded to `on_error`; the recv thread keeps running.
- `oob_frames` is empty for inline-only payloads (primitives, all-inline dicts). No special API path.

### `Publisher`

```cpp
class Publisher {
 public:
  Publisher(Context ctx, DiscoveryClient& discovery,
            std::string topic, std::string cortex_type_name,
            std::uint64_t fingerprint, std::string publisher_node_id,
            std::uint32_t queue_size = 1000,
            std::string endpoint_prefix = "ipc:///tmp/cortex/topics/");

  ~Publisher();   // unregisters from discovery

  // Single-threaded. Returns false on HWM drop; sequence advances regardless.
  bool publish(MetadataBuilder::Frames frames);
  // non-copy, non-move (socket-owning)
};
```

- The ctor slugifies `{node_id}__{topic}.sock` under `endpoint_prefix`, `mkdir -p`s the parent, binds the PUB socket, and registers with the discovery daemon. Any failure throws.
- The dtor calls `unregister_topic` and swallows errors (the daemon may be down on process exit).
- Each `publish()` injects a fresh `MessageHeader{fingerprint, ns_now, seq++}` and emits the multipart message. Inline-only payloads → 3 frames; OOB-bearing payloads → 3 + N.
- ZMQ PUB is single-producer. Callers must serialise.

## Feature parity with Python

✅ supported · ⏭ deliberately omitted · ⚠ planned · n/a doesn't apply to C++.

| Feature | Py | C++ |
|---|:---:|:---:|
| **Publisher** | | |
| Single-producer PUB | ✅ | ✅ |
| Auto-register / unregister with discovery | ✅ | ✅ |
| `auto_register=False` manual mode | ✅ | ⏭ |
| Per-publisher monotonic sequence counter | ✅ | ✅ |
| Non-blocking send (drop on HWM) | ✅ | ✅ |
| Configurable SNDHWM (queue size) | ✅ | ✅ |
| Custom serialiser hooks | ✅ | ⏭ (use `MetadataBuilder` directly) |
| `publish_count` / `dropped_count` accessors | ✅ | ⚠ |
| Thread-safe `publish()` | ❌ | ❌ (same) |
| **Subscriber** | | |
| Async (event-loop) subscriber | ✅ | n/a |
| Threaded subscriber | ✅ | ✅ (default and only mode) |
| Blocking-poll `receive()` | ✅ | ⏭ |
| Discovery lookup at construct (fail-fast) | ✅ | ✅ |
| `wait_for_topic` with timeout | ✅ | ⚠ |
| Strict fingerprint validation | ✅ | ✅ (always) |
| Lax fingerprint validation | ✅ | ⏭ |
| Drop-detection via sequence gap | ✅ | ⚠ |
| Late-joining publisher reconnect | ❌ | ❌ |
| CPU affinity / SCHED_FIFO | ✅ | ⏭ (pin thread externally) |
| **Wire format** | | |
| 24-byte big-endian header | ✅ | ✅ |
| msgpack metadata frame | ✅ | ✅ |
| OOB frames (numpy) | ✅ | ✅ (encoder + decoder) |
| OOB frames (torch) | ✅ | ✅ (decoder; encoder via `pack_torch_oob`, device round-trip best-effort) |
| Inline-only payloads (primitives) | ✅ | ✅ |
| `to_bytes()` single-blob encoder | ✅ | ⏭ |
| `to_frames()` OOB-aware encoder | ✅ | ✅ (via `MetadataBuilder`) |
| Nested OOB walking (Dict / List) | ✅ | ✅ (`DecodedMetadata::walk_oob`) |
| **Runtime composition** | | |
| `Node` composition unit | ✅ | ⏭ (BYO) |
| Async timers (`create_timer`) | ✅ | ⏭ |
| Spawned managed threads | ✅ | ⏭ |
| `run()` / `spin()` lifecycle | ✅ | ⏭ |
| Shared ZMQ context across pubs/subs | ✅ | ✅ (`Context`) |
| **Discovery** | | |
| REQ/REP client | ✅ | ✅ |
| Register / Unregister / Lookup | ✅ | ✅ |
| List / Ping public wrappers | ✅ | ⚠ (encode/decode exists; no accessor) |
| Async lookup | ✅ | ⏭ |
| Auto-reconnect on REQ timeout | ✅ | ✅ |
| **Observability** | | |
| `CORTEX_TRACE_LATENCY` staged timing | ✅ | ⚠ |
| Standard logging | ✅ | via user-supplied `ErrorCallback` |
| `publish_count` / `receive_count` accessors | ✅ | ⚠ |
| Free-threaded CPython advisory | ✅ | n/a |

## Examples

### Inline-only primitive (zero OOB)

```cpp
cortex_wire::Context ctx;
cortex_wire::DiscoveryClient disc(ctx.raw(), "ipc:///tmp/cortex/discovery.sock");

// Subscribe: print each FloatMessage as it arrives.
auto sub = cortex_wire::Subscriber::connect(
    ctx, disc, "battery_voltage",
    cortex_wire::find_by_name("FloatMessage")->fingerprint,
    [](const cortex_wire::Subscriber::Inbound& in) {
      const auto& f = in.metadata.field(0);
      double v = (f.type == msgpack::type::FLOAT64) ? f.via.f64
               : static_cast<double>(f.via.u64);
      std::printf("battery_voltage = %.3f V (seq=%lu)\n",
                  v, in.header.sequence);
    });

// Publish.
cortex_wire::Publisher pub(
    ctx, disc, "battery_voltage", "FloatMessage",
    cortex_wire::find_by_name("FloatMessage")->fingerprint,
    "/my_node");

cortex_wire::MetadataBuilder b(1);
b.pack_double(12.7);
pub.publish(std::move(b).finish());   // 3 frames: [topic, header, metadata]
```

### OOB-bearing array (one numpy buffer)

```cpp
// Publish ArrayMessage{data: float32[4], name, frame_id}.
cortex_wire::MetadataBuilder b(3);
const std::vector<float> values{1.0F, 2.0F, 3.0F, 4.0F};
b.pack_numpy_oob("<f4", {4}, values.data(), values.size() * sizeof(float));
b.pack_str("readings");
b.pack_str("sensor_frame");
pub.publish(std::move(b).finish());   // 4 frames: [topic, header, metadata, oob0]
```

```cpp
// Subscribe: typed zero-copy view over the OOB frame.
[](const cortex_wire::Subscriber::Inbound& in) {
  auto desc = cortex_wire::DecodedMetadata::as_oob(in.metadata.field(0));
  if (!desc || desc->dtype != "<f4") return;
  cortex_wire::OobBuffer<float> view(
      in.oob_frames[desc->buffer_index],
      /*count=*/desc->shape[0]);
  for (float x : view) { /* consume */ }
}
```

### Bypassing discovery for tests

The direct `Subscriber` constructor takes an endpoint string. Useful against a manually-bound PUB socket — see [test/test_pub_sub_roundtrip.cpp](test/test_pub_sub_roundtrip.cpp).

## Roadmap / non-goals

Deferred (⚠ in the table), rough priority:

1. `Subscriber::wait_for_topic(timeout)` — poll discovery for a topic that may register after the subscriber starts.
2. Drop-detection counters on the subscriber side, exposed via accessors.
3. `CORTEX_TRACE_LATENCY` parity (env-var-gated ns-staged timing on recv / decode / callback).
4. Publish / receive count accessors.
5. `DiscoveryClient::list_topics()` and `ping()` wrappers — encode/decode already supports them; just no public accessors yet.

Out of scope indefinitely:

- A `Node` analogue. C++ users have their own composition models; imposing a Cortex-flavoured Node would fight every one of them.
- Late-joining publisher reconnect logic. Python doesn't do this either.
- Custom serialiser plug points. The codec is fixed at msgpack + Cortex's OOB descriptor format; build any payload via `MetadataBuilder` directly.
- Async-runtime variants of Subscriber. Only justified if cortex_wire_cpp ever embeds into asio / coroutine runtimes; the threaded model is sufficient for rclcpp and plain-thread use.
