# cortex_wire_cpp — reference docs

C++ implementation of the Cortex IPC stack: wire codec, discovery client, and
a small high-level pub/sub client. Standalone pure-CMake library, no ROS 2 or
Python dependency.

This document defines what's in the library, what's missing on purpose, and
how the feature surface compares to the Python reference.

## Contents

- [cortex\_wire\_cpp — reference docs](#cortex_wire_cpp--reference-docs)
  - [Contents](#contents)
  - [Scope](#scope)
  - [Wire codec primitives](#wire-codec-primitives)
  - [Discovery client](#discovery-client)
  - [Pub/Sub client](#pubsub-client)
    - [`Context`](#context)
    - [`Subscriber`](#subscriber)
    - [`Publisher`](#publisher)
  - [Feature parity with Python](#feature-parity-with-python)
  - [OOB scope, explicit](#oob-scope-explicit)
  - [Examples](#examples)
    - [Inline-only primitive (zero OOB)](#inline-only-primitive-zero-oob)
    - [OOB-bearing array (one numpy buffer)](#oob-bearing-array-one-numpy-buffer)
    - [Bypassing discovery for tests](#bypassing-discovery-for-tests)
  - [Roadmap / non-goals](#roadmap--non-goals)

---

## Scope

cortex_wire_cpp is a **deliberate subset** of the Python `cortex/core/` package
focused on the needs of plain-C++ consumers — the neurosim ROS 2
bridge, debug tools, sub-100µs control-loop consumers, alternative-language
bridges. It does not try to replicate Python's executor, timer, or
async-event-loop model; a C++ user is expected to bring their own runtime
(rclcpp, std::thread, asio, custom scheduler).

What it provides today:

| Layer        | Components                                                         |
|--------------|--------------------------------------------------------------------|
| Codec        | `MessageHeader`, `DecodedMetadata`, `MetadataBuilder`, `OobBuffer<T>`, `ZmqAllocator<T>` |
| Discovery    | `DiscoveryClient` (REQ/REP, sync)                                  |
| Pub/Sub      | `Context`, `Publisher`, `Subscriber`                               |
| Type catalog | `fingerprint_table` (generated from Python `cortex.messages.standard`) |

## Wire codec primitives

The cortex on-the-wire format is a ZMQ multipart message:

```
frame 0: topic            (bytes)
frame 1: MessageHeader    (24 bytes, big-endian: fp | ts_ns | seq)
frame 2: metadata         (msgpack array of dataclass field values)
frame 3..N: OOB buffers   (raw numpy / torch payloads, one per OOB descriptor
                            referenced inside the metadata)
```

`MessageHeader` ([header.hpp](include/cortex_wire/header.hpp))
: 24-byte fixed header. `from_bytes` / `to_bytes` for codec, public POD layout
  for direct construction. `WireDecodeError` is thrown on under-sized input.

`DecodedMetadata` ([metadata.hpp](include/cortex_wire/metadata.hpp))
: Owns a `msgpack::object_handle` tree decoded from the metadata frame. Random
  access via `field(i)`, plus `as_oob(obj)` to recognise the
  `{"__cortex_oob__": ...}` descriptor maps the Python serialiser emits when a
  numpy/torch value is hoisted out of band. `walk_oob(obj, visitor)` recurses
  into nested dicts/lists for adapters that need every OOB referenced from a
  top-level field (e.g. DictMessage with nested arrays).

`MetadataBuilder` ([metadata.hpp](include/cortex_wire/metadata.hpp))
: Encoder counterpart. `pack_*` helpers for primitives; `pack_numpy_oob` /
  `pack_torch_oob` write the descriptor map inline AND queue the raw buffer
  for emission as the next OOB frame. `finish()` returns
  `{metadata, oob_buffers}`.

`OobBuffer<T>` ([oob_buffer.hpp](include/cortex_wire/oob_buffer.hpp))
: Typed read-only view into a ZMQ frame. Holds a `shared_ptr<zmq::message_t>`
  so the bytes outlive any caller holding the view — no raw-pointer lifetime
  games. `data()`, `size()`, `size_bytes()`, `operator[]`, iterators. Used by
  decoder code to access OOB payloads without copying.

`ZmqAllocator<T>` ([oob_buffer.hpp](include/cortex_wire/oob_buffer.hpp))
: Stateful std-allocator that backs a `std::vector<T, ZmqAllocator<T>>` with
  an existing ZMQ frame. Useful only when you control the vector's allocator
  type. See [oob_buffer.hpp](include/cortex_wire/oob_buffer.hpp) header
  comment for the value-init caveat; **not usable** with stock
  `sensor_msgs/Image::data` (default allocator hard-coded into the generated
  message type).

`fingerprint_table` ([fingerprint_table.hpp](include/cortex_wire/fingerprint_table.hpp))
: Generated map from Cortex's `cortex.messages.standard` catalogue. `find_by_name("DictMessage")` returns `{kind, fingerprint}`. Regenerate with
  [scripts/gen_fingerprint_table.py](scripts/gen_fingerprint_table.py) after
  upstream catalogue changes.

## Discovery client

`DiscoveryClient` ([discovery_client.hpp](include/cortex_wire/discovery_client.hpp))
is a sync REQ/REP client for the Python `cortex-discovery` daemon. One
round-trip per call; sockets are recycled on REQ timeout to recover from the
stuck-after-missed-reply state ZMQ REQ falls into. Supported operations:

- `lookup(topic_name)` — returns `std::optional<TopicInfo>` (nullopt on
  NOT_FOUND). `TopicInfo` carries name, endpoint, message type, fingerprint,
  publisher node id.
- `register_topic(info)` — used by publishers in their ctor.
- `unregister_topic(name)` — tolerates NOT_FOUND; used in publisher dtors.

Low-level `encode_request` / `decode_response` are exposed for unit tests.

## Pub/Sub client

Three classes form the high-level client: [`Context`](include/cortex_wire/context.hpp),
[`Publisher`](include/cortex_wire/publisher.hpp),
[`Subscriber`](include/cortex_wire/subscriber.hpp).

### `Context`

```cpp
class Context {
 public:
  Context();                                  // owns a fresh zmq::context_t(1)
  explicit Context(zmq::context_t& external); // wrap externally-owned, no dtor cleanup
  zmq::context_t& raw() noexcept;
};
```

Copyable, refcounted shared ownership. Last destruction runs
`shutdown()` + `close()` in the right order to unblock recv()s.

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
  Subscriber(Context ctx,
             std::string endpoint,
             std::string topic,
             std::uint64_t expected_fingerprint,
             MessageCallback on_message,
             ErrorCallback   on_error = {});

  // Factory — does the discovery lookup, throws on not-found or fp mismatch.
  static Subscriber connect(Context ctx,
                            DiscoveryClient& discovery,
                            std::string topic,
                            std::uint64_t expected_fingerprint,
                            MessageCallback on_message,
                            ErrorCallback   on_error = {});

  ~Subscriber();                              // joins recv thread (≤100 ms)
  // non-copy, non-move (thread-owning)
};
```

Semantics:

- Ctor spawns a dedicated recv thread; it is running on return.
- Always **strict fingerprint**. Messages whose wire header fingerprint
  differs from `expected_fingerprint` are dropped and reported via
  `on_error`. Python's "lax" mode is intentionally omitted.
- The callback runs on the recv thread, one message at a time. Exceptions
  thrown by the callback are caught and forwarded to `on_error` (the recv
  thread keeps running).
- `oob_frames` may be empty for inline-only payloads (primitives, all-inline
  dicts). No special API path.

### `Publisher`

```cpp
class Publisher {
 public:
  Publisher(Context ctx,
            DiscoveryClient& discovery,
            std::string topic,
            std::string cortex_type_name,         // e.g. "DictMessage"
            std::uint64_t fingerprint,
            std::string publisher_node_id,
            std::uint32_t queue_size = 1000,
            std::string endpoint_prefix = "ipc:///tmp/cortex/topics/");

  ~Publisher();                                   // unregisters from discovery

  // Single-threaded send. Caller serialises.
  // Returns false if ZMQ HWM drop occurred (sequence advances regardless,
  // matching Python).
  bool publish(MetadataBuilder::Frames frames);
  // non-copy, non-move (socket-owning)
};
```

Semantics:

- Ctor slugifies `{node_id}__{topic}.sock` under `endpoint_prefix`, creates
  the parent directory if missing, binds the PUB socket, and registers with
  the discovery daemon. Any failure throws.
- Dtor calls `unregister_topic` and swallows errors (the daemon may be down
  on process exit).
- Each `publish()` injects a fresh `MessageHeader{fingerprint, ns_now,
  seq++}`, then emits the multipart message. Inline-only payloads send 3
  frames; OOB-bearing payloads send 3 + N.
- ZMQ PUB is single-producer. Callers must serialise; do not call from
  multiple threads.

## Feature parity with Python

Legend: ✅ supported, ⏭ deliberately omitted, ⚠ planned for a later phase,
n/a doesn't apply to C++.

| Feature                                              | Python `cortex/core/` | C++ `cortex_wire_cpp` |
|------------------------------------------------------|:---:|:---:|
| **Publisher**                                        |     |     |
| Single-producer PUB                                  | ✅ | ✅ |
| Auto-register / unregister with discovery            | ✅ | ✅ |
| `auto_register=False` manual mode                    | ✅ | ⏭ |
| Per-publisher monotonic sequence counter             | ✅ | ✅ |
| Non-blocking send (drop on HWM)                      | ✅ | ✅ |
| Configurable SNDHWM (queue size)                     | ✅ | ✅ (ctor arg) |
| Custom serialiser hooks                              | ✅ | ⏭ (use `MetadataBuilder` directly) |
| `publish_count` / `dropped_count` properties         | ✅ | ⚠ later |
| Thread-safe `publish()`                              | ❌ | ❌ (same) |
| **Subscriber**                                       |     |     |
| Async (event-loop) subscriber                        | ✅ | n/a |
| Threaded subscriber                                  | ✅ | ✅ (default and only mode) |
| Blocking-poll `receive()`                            | ✅ | ⏭ |
| Discovery lookup at construct (fail-fast)            | ✅ | ✅ |
| `wait_for_topic` with timeout                        | ✅ | ⚠ later |
| Strict fingerprint validation                        | ✅ | ✅ (always) |
| Lax fingerprint validation                           | ✅ | ⏭ |
| Drop-detection via sequence gap                      | ✅ | ⚠ later |
| Late-joining publisher (reconnect)                   | ❌ | ❌ |
| CPU affinity / SCHED_FIFO                            | ✅ | ⏭ (pin thread externally) |
| **Wire format / serialisation**                      |     |     |
| 24-byte big-endian header                            | ✅ | ✅ |
| msgpack metadata frame                               | ✅ | ✅ |
| OOB frames for numpy arrays                          | ✅ | ✅ (decoder + encoder) |
| OOB frames for torch tensors                         | ✅ | ✅ (decoder; encoder via `pack_torch_oob`, device round-trip best effort) |
| Inline-only payloads (primitives)                    | ✅ | ✅ |
| `to_bytes()` inline-only single-blob encoder         | ✅ | ⏭ |
| `to_frames()` OOB-aware encoder                      | ✅ | ✅ (via `MetadataBuilder`) |
| Nested OOB walking (Dict / List)                     | ✅ | ✅ (`DecodedMetadata::walk_oob`) |
| **Runtime composition**                              |     |     |
| `Node` composition unit                              | ✅ | ⏭ (bring your own) |
| Async timers (`create_timer`)                        | ✅ | ⏭ |
| Spawned managed threads                              | ✅ | ⏭ |
| `run()` / `spin()` lifecycle                         | ✅ | ⏭ |
| Shared ZMQ context across pubs/subs                  | ✅ | ✅ (`Context`) |
| **Discovery**                                        |     |     |
| REQ/REP client                                       | ✅ | ✅ |
| Register / Unregister / Lookup / List / Ping         | ✅ | partial — Register / Unregister / Lookup |
| Async lookup                                         | ✅ | ⏭ (sync only) |
| Auto-reconnect on REQ timeout                        | ✅ | ✅ |
| **Observability**                                    |     |     |
| `CORTEX_TRACE_LATENCY` ns-resolution staged timing   | ✅ | ⚠ later |
| Standard logging                                     | ✅ | via user-supplied `ErrorCallback` |
| `publish_count` / `receive_count` properties         | ✅ | ⚠ later |
| Free-threaded CPython advisory                       | ✅ | n/a |

## OOB scope, explicit

**OOB frames are optional and variadic.** The library treats the multipart
message as `header + metadata + zero-or-more OOB`, never as a required
OOB-bearing protocol.

- A `Subscriber` callback receives `oob_frames` as a (possibly empty)
  `std::vector<ZmqFramePtr>`. Decoders consume zero, one, or many.
- `Publisher::publish()` accepts a `MetadataBuilder::Frames` whose
  `oob_buffers` may be empty. The publisher emits exactly 3 frames in that
  case.
- Inline-only messages (`StringMessage "hello"`, `FloatMessage 3.14`,
  `DictMessage {"a": 1, "b": [1,2,3]}` with no numpy fields) round-trip with
  zero OOB. Nested numpy values inside a DictMessage each generate one OOB
  descriptor + one frame, and `DecodedMetadata::walk_oob` recursively
  enumerates them.

So: **the C++ client is no more or less OOB-restricted than the Python
client.** The same set of `cortex.messages.standard` types round-trips
either way.

## Examples

### Inline-only primitive (zero OOB)

```cpp
cortex_wire::Context ctx;
cortex_wire::DiscoveryClient disc(ctx.raw(), "ipc:///tmp/cortex/discovery.sock");

// Subscriber side — print each FloatMessage as it arrives.
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

// Publisher side.
cortex_wire::Publisher pub(
    ctx, disc, "battery_voltage", "FloatMessage",
    cortex_wire::find_by_name("FloatMessage")->fingerprint,
    "/my_node");

cortex_wire::MetadataBuilder b(1);   // one msgpack field
b.pack_double(12.7);
pub.publish(std::move(b).finish()); // 3 frames total: [topic, header, metadata]
```

### OOB-bearing array (one numpy buffer)

```cpp
// Publisher: ArrayMessage with a float32 vector.
cortex_wire::MetadataBuilder b(3);   // ArrayMessage = data, name, frame_id
const std::vector<float> values{1.0F, 2.0F, 3.0F, 4.0F};
b.pack_numpy_oob("<f4", {4}, values.data(), values.size() * sizeof(float));
b.pack_str("readings");
b.pack_str("sensor_frame");
pub.publish(std::move(b).finish()); // 4 frames: [topic, header, metadata, oob0]
```

```cpp
// Subscriber side — typed zero-copy view over the OOB frame.
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

The direct `Subscriber` constructor accepts an endpoint string. Useful when
you're running against a manually-bound PUB socket (e.g. unit tests; see
[test/test_pub_sub_roundtrip.cpp](test/test_pub_sub_roundtrip.cpp)).

## Roadmap / non-goals

Deferred (⚠ later in the table), rough priority:

1. `Subscriber::wait_for_topic(timeout)` — poll discovery for a topic that
   may register after the subscriber starts.
2. Drop-detection counters on the subscriber side, exposed via accessors.
3. `CORTEX_TRACE_LATENCY` parity (env-var-gated ns-staged timing on
   recv/decode/callback).
4. Publish/receive count accessors.
5. `DiscoveryClient::list_topics()` and `ping()` wrappers (encode/decode
   already supports them; just no public accessors yet).

Out of scope indefinitely:

- A `Node` analogue. C++ users have their own composition models; imposing a
  Cortex-flavoured Node would fight every one of them.
- Late-joining publisher reconnect logic. Python doesn't do this either.
- Custom serialiser plug points. The codec is fixed at msgpack + Cortex's
  OOB descriptor format; consumers can construct any payload via
  `MetadataBuilder` directly.
- Async-runtime variants of Subscriber. Justified only if cortex_wire_cpp
  ever embeds into asio/coroutine runtimes; the threaded model is sufficient
  for rclcpp and plain-thread use.
