# Plan: Cortex ↔ ROS 2 Bridge

A pair of native ROS 2 nodes (`cortex_to_ros2`, `ros2_to_cortex`) that bridge Cortex's typed pub/sub topics to ROS 2 topics on the same host. Configured through a single clean YAML schema, deployed via launch files using **composable nodes** (intra-process / loaned messages) so that bridged data can move between the bridge and downstream ROS 2 consumers without an additional copy.

Reference for inspiration only — not the target API or quality bar: <https://github.com/grasp-lyrl/neurosim/tree/main/src/neurosim/cortex/ros2_ws/zmq_ros2_bridge>. That implementation:

- hard-codes 5 message kinds in C++ (`enable_imu`, `enable_color`, ...),
- spins one std::thread per topic with raw `nlohmann::json` decoding,
- ships a single non-composable `Node`, no zero-copy considered,
- never uses Cortex's actual wire format (24-byte header + msgpack metadata + OOB frames) — it reinvents a separate JSON / "numpy parts" wire.

We will do better: drive the bridge from Cortex's own message registry, decode through the canonical `Message.from_frames` path, expose the bridge as composable components, and treat zero-copy as a first-class deployment mode.

---

## 1. Goals & non-goals

### Goals

1. **Bidirectional** bridging of Cortex's standard message types (everything in [src/cortex/messages/standard.py](../src/cortex/messages/standard.py)) to/from canonical ROS 2 types (`sensor_msgs`, `geometry_msgs`, `std_msgs`, `tf2_msgs`).
2. A **single declarative YAML** describing every bridged topic, with sane defaults and per-topic QoS overrides. No C++ enable-flag explosion.
3. **Composable nodes** so the bridge can be loaded into a component container alongside downstream ROS 2 nodes — enabling intra-process (`use_intra_process_comms=True`) and, where the ROS 2 middleware supports it, **loaned-message** zero-copy.
4. **Zero-copy on the Cortex side** — reuse the OOB frame memoryviews already exposed by `Message.from_frames`; never `.tobytes()` array payloads.
5. **Minimal new dependencies on the Cortex side.** The bridge lives in its own ROS 2 package; building Cortex itself does not require a ROS 2 install.
6. **Clean shutdown, clean restarts**, no zombie zmq contexts, no leaked discovery registrations.
7. **One executable per direction**, both implemented as composable components, both runnable as plain `ros2 run` executables for development.

### Non-goals (for v1)

- Bridging *custom* user-defined Cortex messages (no `RobotState`-style classes) — only the standard catalogue. Custom types can be added by extending the registry; the design must not block this, but auto-derivation is out of scope.
- Bridging ROS 2 services or actions. Topics only.
- TCP / cross-host transport. The bridge assumes Cortex IPC and a co-located ROS 2 graph. (Multi-machine is on Cortex's broader roadmap — see [docs/TODO](../docs/TODO).)
- DDS-level tuning (RMW QoS profiles beyond what YAML exposes), security profiles, lifecycle nodes.

---

## 2. What "zero-copy" actually buys us, and where

ROS 2 zero-copy has two distinct mechanisms; the bridge needs both to give a single end-to-end story:

| Layer                          | Mechanism                                                                                              | Win                                                                |
| ------------------------------ | ------------------------------------------------------------------------------------------------------ | ------------------------------------------------------------------ |
| Cortex IPC → bridge process    | ZMQ multipart with OOB frames; `np.frombuffer(frame.buffer, ...)` aliases ZMQ memory (no copy)         | Avoids 1 copy per array on ingest                                  |
| Bridge → ROS 2 in-process consumer | Composable node container + `use_intra_process_comms=True`                                          | Avoids serialize/deserialize hop and shared_ptr message is passed by reference |
| Bridge → ROS 2 cross-process consumer (advanced) | `rmw_zero_copy_cyclonedds` / Iceoryx loaned messages via `publisher->borrow_loaned_message()` | Avoids DDS network stack copy; needs shared-mem RMW                |

The bridge implementation must:

1. Avoid heap allocation per message for the data buffer. The `ZmqAllocator<T>` introduced in PR2 lets a `std::vector<uint8_t>` use a ZMQ frame's buffer as its storage with no fresh allocation. **Caveat discovered in PR2**: `std::vector`'s sizing constructor value-initialises its elements (zeros them), so the published `Image::data` requires one explicit memcpy from the OOB frame into `v.data()`. The wins are (a) no `malloc` per message and (b) the frame's lifetime is tied to the vector's lifetime for free. *True* byte-level aliasing into a default-allocator `sensor_msgs::Image::data` is not achievable without loaned messages or a custom message type; see §14.
2. Publish using `std::unique_ptr<Msg>` (the API path that `rclcpp` requires for intra-process zero-copy with multiple subscribers).
3. Detect at runtime whether `RMW_IMPLEMENTATION` supports loaned messages (`can_loan_messages()`), and fall back gracefully if not.

The launch file must support loading the bridge **inside** a `ComponentContainer` together with downstream ROS 2 nodes — this is where intra-process composition actually pays off. A standalone `ros2 run` executable is provided for development/debug, but production deployments compose.

---

## 3. Language choice: C++

**Decision: C++ from v1.** Python is not viable.

Reasoning:

- `rclpy` does not participate in `rclcpp_components` containers in any way that delivers zero-copy. Intra-process comms with serialization elision is an `rclcpp`-only path. A Python bridge would always serialize on egress, defeating the goal in §2.
- Composability is non-negotiable for this design — the point of the bridge is to let downstream ROS 2 consumers receive Cortex data without an extra copy, which requires co-loading into a `ComponentContainer` that only accepts `rclcpp` plugins.
- Loaned-message paths (`borrow_loaned_message()` on Iceoryx-class RMWs) are also C++-first; `rclpy` support is partial-to-nonexistent depending on distro.

The cost is duplicating Cortex's wire format on the C++ side. This is tractable: the wire is small, stable, and **the bridge only needs decoders for the standard catalogue** (no introspection of user-defined types — see §3.1). The duplication is bounded.

### 3.1 What needs reimplementing in C++

A self-contained `cortex_wire/` C++ library inside the package, no dependency on the Python cortex install:

- **`MessageHeader`** — 24 bytes, big-endian `(fingerprint u64, timestamp_ns u64, sequence u64)`. Trivial. Lives in `cortex_wire/header.hpp`.
- **Fingerprint table** — hand-maintained `constexpr` map from fingerprint → message-kind enum, generated once by a Python script that imports the standard catalogue and prints the table. Committed in-tree, regenerated when the standard catalogue changes (CI checks staleness).
- **Metadata frame decoder** — msgpack-c (header-only, ament-packageable). The metadata frame is a small msgpack array of ordered field values; arrays/tensors appear as `{"__cortex_oob__": "numpy", "buffer": i, "dtype": "...", "shape": [...]}` dicts. Decoder walks the array per declared schema for each message kind.
- **OOB buffer handoff** — `zmq::message_t` already owns the buffer; we hand it to a `std::shared_ptr<zmq::message_t>` and let the ROS 2 message's `data` field hold a non-owning view into it (via a custom `Allocator` that releases the shared_ptr on destruction). This is the key to zero-copy: the ZMQ frame's memory backs the published `sensor_msgs/Image::data` for its entire ROS-side lifetime.

### 3.2 Custom user message types (later)

The standard catalogue covers v1. For users to bridge their own Cortex messages, two paths are possible later:

1. Hand-write a C++ adapter mirroring the Python dataclass (verbose but explicit; same approach as ROS 2 `.msg` files).
2. Code-gen the C++ struct + decoder from the Python dataclass via a small build-time tool. Defer to v2.

Neither blocks v1; the architecture must just not paint itself into a corner. The adapter registry uses runtime lookup keyed by fingerprint, so adding new types later is a new file plus a registry entry.

---

## 4. Package layout

```
ros2_bridge/
├── PLAN.md                              ← this document
├── cortex_ros2_bridge/                  ← ament_cmake ROS 2 package
│   ├── package.xml
│   ├── CMakeLists.txt
│   ├── include/cortex_ros2_bridge/
│   │   ├── cortex_wire/
│   │   │   ├── header.hpp               ← 24-byte MessageHeader
│   │   │   ├── fingerprint_table.hpp    ← auto-generated fingerprint → kind enum
│   │   │   ├── metadata.hpp             ← msgpack metadata frame decoder
│   │   │   └── oob_buffer.hpp           ← shared zmq::message_t ownership helpers
│   │   ├── config.hpp                   ← YAML schema dataclass-equivalent + loader
│   │   ├── qos.hpp                      ← YAML → rclcpp::QoS mapping
│   │   ├── registry.hpp                 ← adapter registry (kind+ros_type → factory)
│   │   ├── adapter.hpp                  ← AdapterBase template
│   │   └── adapters/
│   │       ├── primitives.hpp
│   │       ├── arrays.hpp
│   │       ├── image.hpp
│   │       ├── pointcloud.hpp
│   │       ├── pose.hpp
│   │       ├── transform.hpp
│   │       ├── tensor.hpp
│   │       ├── header.hpp
│   │       └── multi.hpp
│   ├── src/
│   │   ├── cortex_to_ros2_node.cpp      ← rclcpp_components plugin
│   │   ├── ros2_to_cortex_node.cpp      ← rclcpp_components plugin
│   │   ├── config.cpp
│   │   ├── registry.cpp
│   │   ├── cortex_wire/metadata.cpp
│   │   └── adapters/*.cpp               ← one TU per adapter
│   ├── scripts/
│   │   └── gen_fingerprint_table.py     ← imports cortex.messages, prints C++ header
│   ├── config/
│   │   ├── example_minimal.yaml
│   │   ├── example_full.yaml
│   │   └── schema.json                  ← optional JSON schema for editors
│   ├── launch/
│   │   ├── cortex_to_ros2.launch.py
│   │   ├── ros2_to_cortex.launch.py
│   │   ├── bidirectional.launch.py
│   │   └── composable_container.launch.py   ← loads both bridge components into one container
│   └── test/
│       ├── test_config.cpp
│       ├── test_wire_decode.cpp
│       ├── test_adapters_roundtrip.cpp
│       └── test_launch_smoke.py         ← launch_testing
└── README.md
```

`cortex_ros2_bridge` is an `ament_cmake` package. The two components are registered via `rclcpp_components_register_nodes(...)` so they can be loaded into any standard `component_container_mt`. Plain executables (`cortex_to_ros2`, `ros2_to_cortex`) are emitted via `rclcpp_components_register_node(... EXECUTABLE ...)` for development/debug runs.

### 4.1 Dependencies

- `rclcpp`, `rclcpp_components`
- `sensor_msgs`, `geometry_msgs`, `std_msgs`, `builtin_interfaces`, `tf2_msgs`, `tf2_ros`
- `cppzmq` (header-only wrapper over libzmq)
- `msgpack-cxx` (header-only msgpack-c)
- `yaml-cpp`
- Build-time: Python 3 with `cortex` installed, for `scripts/gen_fingerprint_table.py` to run at configure time (or pre-checked-in output if Python cortex is unavailable on the build host).

---

## 5. Configuration: a single clean YAML

The neurosim approach (flat `enable_X` parameters, per-message hard-coded fields) does not scale to Cortex's standard catalogue and is impossible to extend cleanly. We replace it with a structured schema.

### 5.1 Schema (v1)

```yaml
# Cortex ↔ ROS 2 bridge configuration
version: 1

cortex:
  # Discovery daemon address; defaults to ipc:///tmp/cortex_discovery
  discovery_address: "ipc:///tmp/cortex_discovery"
  node_name_prefix: "cortex_bridge"

defaults:
  # Applied to every direction unless overridden per-topic
  qos:
    reliability: reliable      # reliable | best_effort
    durability: volatile       # volatile | transient_local
    history: keep_last         # keep_last | keep_all
    depth: 10
  loaned_messages: auto        # auto | force | never (use rmw loaned msgs when available)

# Cortex → ROS 2 bridges
cortex_to_ros2:
  - name: lidar_points
    cortex:
      topic: "/sensor/lidar/points"
      type: PointCloudMessage      # name from cortex.messages.standard
    ros2:
      topic: "/lidar/points"
      type: "sensor_msgs/msg/PointCloud2"   # optional; default chosen by adapter
      frame_id: "lidar"                     # static override; else use msg.frame_id
    qos:
      reliability: best_effort
      depth: 5

  - name: camera_image
    cortex:
      topic: "/camera/rgb"
      type: ImageMessage
    ros2:
      topic: "/camera/image_raw"
      # type inferred from adapter (sensor_msgs/Image)

  - name: robot_pose
    cortex:
      topic: "/state/pose"
      type: PoseMessage
    ros2:
      topic: "/robot/pose"
      broadcast_tf: true           # adapter option: also publish to /tf
      tf_parent: "map"

# ROS 2 → Cortex bridges
ros2_to_cortex:
  - name: cmd_vel
    ros2:
      topic: "/cmd_vel"
      type: "geometry_msgs/msg/Twist"
    cortex:
      topic: "/control/cmd"
      type: PoseMessage            # adapter handles the lossy mapping
    qos:
      reliability: reliable
      depth: 1
      durability: volatile

  - name: clock
    ros2:
      topic: "/clock"
      type: "rosgraph_msgs/msg/Clock"
    cortex:
      topic: "/sim/time"
      type: TimestampMessage
```

### 5.2 What the loader does

1. Parses YAML and validates against a Pydantic / `dataclass`-backed schema in `config.py`. Invalid entries fail loudly *at launch*, never silently at runtime.
2. Resolves Cortex `type` strings to actual classes through `cortex.messages.MessageType.get_all()` (which already keys by class — we add a name→class lookup) so users cannot reference messages that are not registered.
3. Resolves ROS 2 `type` strings via `rclpy.utilities.get_message` or by importing the module; if `type` is omitted, the adapter's default ROS type is used.
4. Confirms that an adapter is registered for each (cortex_type → ros2_type) pair; otherwise refuses to start. No silent passthrough.
5. Stamps each bridge entry with a unique `name` used in logs, parameter overrides, and metrics labels.

### 5.3 Why this schema (vs. neurosim's flat `enable_X`)

- **Open set of types.** Neurosim's flat schema cannot describe a second IMU without editing C++. Ours describes *N* bridges, each a (cortex_topic, ros2_topic, type, qos) tuple.
- **QoS is per-bridge.** Image streams need `best_effort`; control commands need `reliable, depth=1`. Mixing them in one process requires per-entry QoS.
- **Adapters declare their own types.** The `ImageMessage` adapter knows its ROS 2 counterpart is `sensor_msgs/Image`. Users only specify it when overriding.
- **No silent failures.** Validation happens at config load, not at first message arrival.

---

## 6. Adapter registry

Each (Cortex type ↔ ROS 2 type) pair is one **Adapter**. C++ template:

```cpp
// adapter.hpp
template <typename CortexKind, typename Ros2Msg>
struct Adapter {
    static constexpr CortexMessageKind kind = CortexKind::kind;
    using ros2_type = Ros2Msg;

    // Cortex → ROS 2. `frames` are the zmq::message_t parts after the topic frame
    // (header, metadata, oob0..). The returned unique_ptr is what rclcpp wants
    // for the intra-process zero-copy publish path.
    static std::unique_ptr<Ros2Msg> to_ros2(
        const MessageHeader & header,
        const DecodedMetadata & meta,
        std::vector<std::shared_ptr<zmq::message_t>> & oob,
        const BridgeEntry & cfg);

    // ROS 2 → Cortex. Builds the multipart frames for transport.
    static std::vector<zmq::message_t> to_cortex(
        const Ros2Msg & msg,
        uint64_t sequence,
        const BridgeEntry & cfg);
};
```

Adapters register themselves into a runtime table keyed by `(CortexMessageKind, ros2_type_name)`:

```cpp
REGISTER_ADAPTER(ImageAdapter, CortexMessageKind::Image, "sensor_msgs/msg/Image");
```

### 6.1 Zero-copy in the Image adapter

The critical adapter. Sketch:

```cpp
std::unique_ptr<sensor_msgs::msg::Image> ImageAdapter::to_ros2(
    const MessageHeader & header,
    const DecodedMetadata & meta,
    std::vector<std::shared_ptr<zmq::message_t>> & oob,
    const BridgeEntry & cfg)
{
    auto out = std::make_unique<sensor_msgs::msg::Image>();
    out->header.stamp    = ns_to_time(header.timestamp_ns);
    out->header.frame_id = cfg.frame_id.value_or("");
    out->height          = meta.fields["height"].as<uint32_t>();
    out->width           = meta.fields["width"].as<uint32_t>();
    out->encoding        = meta.fields["encoding"].as<std::string>();
    out->is_bigendian    = 0;
    out->step            = meta.fields["data"].oob_stride0();

    // The raw image bytes live in oob[0] — a zmq::message_t we already own
    // via shared_ptr. Hand its memory to the ROS message via a custom
    // allocator that keeps the shared_ptr alive for the message's lifetime.
    auto buf = oob[0];
    out->data = ZmqBackedVector<uint8_t>(
        static_cast<uint8_t *>(buf->data()), buf->size(), buf);
    return out;
}
```

`ZmqBackedVector<T>` is a thin wrapper around `std::vector` that uses a stateful allocator carrying the `shared_ptr<zmq::message_t>`. The vector never copies — its `data()` pointer is the ZMQ frame's buffer; deallocation releases the `shared_ptr`. The published ROS message owns one reference to the ZMQ frame, and any intra-process subscriber receiving the `unique_ptr` (or a `const &` to a shared one) sees the same memory.

For loaned-message RMWs, the adapter has an alternate path: `borrow_loaned_message<sensor_msgs::msg::Image>()`, `memcpy` from the ZMQ frame into the loaned buffer, and `publish(std::move(loaned))`. This costs one copy but enables shared-memory cross-process delivery.

The adapter registry is the *only* place that knows about specific ROS 2 message types — adding `CompressedImage` is one new adapter source file plus one schema entry.

### 6.1 Standard catalogue → ROS 2 mapping

| Cortex                  | ROS 2 (default)                          | Notes                                                            |
| ----------------------- | ---------------------------------------- | ---------------------------------------------------------------- |
| `StringMessage`         | `std_msgs/String`                        | Trivial                                                          |
| `IntMessage`            | `std_msgs/Int64`                         |                                                                  |
| `FloatMessage`          | `std_msgs/Float64`                       |                                                                  |
| `BytesMessage`          | `std_msgs/ByteMultiArray`                |                                                                  |
| `DictMessage`           | `std_msgs/String` (JSON-encoded)         | Opt-in only; warn loudly. Better: ask user to define a custom msg. |
| `ListMessage`           | `std_msgs/String` (JSON-encoded)         | Same caveat                                                      |
| `ArrayMessage`          | `std_msgs/Float32MultiArray` (and friends, per dtype) | Adapter dispatches on `arr.dtype`                  |
| `MultiArrayMessage`     | (none — must be split per key)           | YAML must specify the per-key target topic                       |
| `TensorMessage`         | `std_msgs/Float32MultiArray`             | Tensors are CPU'd on Cortex side already                         |
| `MultiTensorMessage`    | (none — same as MultiArray)              |                                                                  |
| `ImageMessage`          | `sensor_msgs/Image`                      | Zero-copy data buffer                                            |
| `PointCloudMessage`     | `sensor_msgs/PointCloud2`                | Pack fields per attribute presence (xyz, rgb, intensity, normals); zero-copy where possible |
| `PoseMessage`           | `geometry_msgs/PoseStamped`              | `frame_id` from msg or YAML                                       |
| `TransformMessage`      | `geometry_msgs/TransformStamped` (+ optional `/tf` broadcast) | adapter decomposes 4×4 → translation + quaternion       |
| `TimestampMessage`      | `builtin_interfaces/Time`                |                                                                  |
| `HeaderMessage`         | `std_msgs/Header`                        |                                                                  |

For each row, we also implement the reverse direction except where lossy (the JSON-encoded dict path is one-way by default, opt-in only).

---

## 7. Bridge components

Both are `rclcpp::Node` subclasses registered with `RCLCPP_COMPONENTS_REGISTER_NODE(...)`. Loaded into a `component_container_mt` they participate fully in intra-process comms (`use_intra_process_comms: true` on the container).

### 7.1 `CortexToRos2Bridge` (component)

Responsibilities:

1. Construct a single `zmq::context_t` and a discovery client. The discovery client talks REQ/REP to the daemon to look up each Cortex topic's IPC endpoint, exactly as the Python `Subscriber` does (the discovery protocol is a small msgpack REQ/REP — port to C++ alongside the wire library).
2. For each `cortex_to_ros2` YAML entry:
   - Create a `zmq::socket_t` SUB connected to the discovered endpoint, subscribed to the topic-name frame.
   - Verify fingerprint by reading the first message header and asserting it matches the expected `CortexMessageKind`; refuse to start the bridge entry on mismatch (always strict).
   - Create an `rclcpp::Publisher<Ros2Msg>` using the YAML-derived `rclcpp::QoS`.
   - Look up the adapter in the registry by `(kind, ros2_type_name)`.
3. Run a dedicated `std::jthread` per SUB socket (one OS thread per Cortex topic). Each thread loops on `recv_multipart` and calls the adapter, then `publisher_->publish(std::move(unique_ptr_msg))`. Per-topic threads are cheap, keep slow topics from blocking fast ones, and map naturally onto Cortex's per-topic socket model.
4. Use a single shared `zmq::poller_t` instead of N threads when the number of bridge entries is large (>16). The threshold is an internal knob, not exposed; both code paths exist behind the same `RecvLoop` interface.
5. On destruction: signal stop, join threads, close sockets, term context.

Publishing from a non-rclcpp thread is supported (`rclcpp::Publisher::publish` is thread-safe). Intra-process delivery still works: rclcpp's intra-process manager dispatches the `unique_ptr` onto subscribers' executors regardless of which thread called `publish`.

### 7.2 `Ros2ToCortexBridge` (component)

Mirror direction.

1. One `zmq::context_t`, one PUB socket per Cortex topic, register each with the discovery daemon.
2. For each `ros2_to_cortex` entry, create an `rclcpp::Subscription<Ros2Msg>` with the YAML QoS, callback bound to the adapter's `to_cortex(...)`.
3. The callback runs on the rclcpp executor thread, builds the multipart frames via the adapter, and `socket.send_multipart(...)`. PUB sockets aren't thread-safe, so each topic's PUB socket is only ever touched from the rclcpp executor — the `MultiThreadedExecutor` mutex-guards the callback group per subscription.
4. Sequence numbers are per-publisher monotonic, matching Cortex's Python publisher semantics.

### 7.3 Composable node container

`composable_container.launch.py` loads both components into a single `component_container_mt` with `use_intra_process_comms=True`. Downstream ROS 2 nodes the user wants colocated can be added to the same container (their launch file extends ours, or copies the `ComposableNode` descriptor) and will receive bridge-published messages **without** serialization.

Development run (no container, no intra-process):

```bash
ros2 run cortex_ros2_bridge cortex_to_ros2 --ros-args -p config_path:=/tmp/bridge.yaml
```

Production run:

```bash
ros2 launch cortex_ros2_bridge composable_container.launch.py config:=/tmp/bridge.yaml
```

---

## 8. Launch files

### 8.1 `composable_container.launch.py` (the main one)

```python
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import ComposableNodeContainer
from launch_ros.descriptions import ComposableNode


def generate_launch_description():
    config_arg = DeclareLaunchArgument(
        "config", description="Path to bridge YAML config")
    container_name_arg = DeclareLaunchArgument(
        "container_name", default_value="cortex_bridge_container")

    config = LaunchConfiguration("config")

    container = ComposableNodeContainer(
        name=LaunchConfiguration("container_name"),
        namespace="",
        package="rclcpp_components",
        executable="component_container_mt",
        composable_node_descriptions=[
            ComposableNode(
                package="cortex_ros2_bridge",
                plugin="cortex_ros2_bridge::CortexToRos2Bridge",
                name="cortex_to_ros2",
                parameters=[{"config_path": config}],
                extra_arguments=[{"use_intra_process_comms": True}],
            ),
            ComposableNode(
                package="cortex_ros2_bridge",
                plugin="cortex_ros2_bridge::Ros2ToCortexBridge",
                name="ros2_to_cortex",
                parameters=[{"config_path": config}],
                extra_arguments=[{"use_intra_process_comms": True}],
            ),
        ],
        output="screen",
    )

    return LaunchDescription([config_arg, container_name_arg, container])
```

### 8.2 Single-direction launch files

`cortex_to_ros2.launch.py` and `ros2_to_cortex.launch.py` load only one component into the container. Useful when the user only needs one direction or wants to colocate the bridge with their own composable node graph in their own launch file (they can copy the `ComposableNode` descriptor).

### 8.3 Standalone (debug) executables

`cortex_to_ros2` and `ros2_to_cortex` are also entry points in `setup.py`, so they run without a container. No intra-process zero-copy in this mode — useful only for debugging.

---

## 9. Implementation phases (PR-sized)

Each phase ends with passing tests and a working YAML example.

### PR1 — Package skeleton & config loader

- Create the `cortex_ros2_bridge` ament_cmake package, `package.xml`, `CMakeLists.txt`, `colcon build` green.
- Implement `config.hpp`/`.cpp` — yaml-cpp parser, schema validation, structured errors.
- gtest cases for valid/invalid YAML.
- No ZMQ or adapters yet.

### PR2 — Cortex wire library (C++)

- Port `MessageHeader` (24 bytes, big-endian) and `DecodedMetadata` (msgpack-cxx).
- Port the discovery REQ/REP protocol just enough to look up a topic's endpoint.
- `scripts/gen_fingerprint_table.py` introspects the Python standard catalogue and emits `fingerprint_table.hpp`. CMake invokes it at configure time; result is also checked in for builds without a Python cortex install.
- gtest decode against fixture frames captured from a Python publisher (golden test).
- `ZmqBackedVector<T>` allocator + lifetime tests.

### PR3 — Adapter base + primitives

- `Adapter` template + `REGISTER_ADAPTER` macro + runtime registry.
- Implement `StringMessage`, `IntMessage`, `FloatMessage`, `BytesMessage`, `TimestampMessage`, `HeaderMessage`.
- Pure-conversion round-trip tests (no ZMQ, no rclcpp executor).

### PR4 — `CortexToRos2Bridge` MVP

- Component class, per-topic thread, recv loop, adapter dispatch.
- `RCLCPP_COMPONENTS_REGISTER_NODE` registration; plain executable target via `rclcpp_components_register_node(... EXECUTABLE ...)`.
- `cortex_to_ros2.launch.py` with this component.
- launch_testing smoke: Python cortex publisher in a subprocess → bridge → assert ROS topic payload.

### PR5 — `Ros2ToCortexBridge` MVP

- Symmetric component, rclcpp subscription → adapter → zmq PUB.
- launch_testing smoke in reverse: Python cortex subscriber receives data published from a ROS `topic pub`.

### PR6 — Array / Image / PointCloud adapters

- Implement `ArrayMessage`, `MultiArrayMessage`, `ImageMessage`, `PointCloudMessage` (both directions).
- Verify zero-copy on Cortex→ROS 2: assert `sensor_msgs::Image::data.data()` == `zmq::message_t::data()` (pointer equality) in a test fixture.
- `bench_bridge_throughput.cpp` measuring 1920×1080 RGB at 60 Hz: end-to-end latency, CPU, and `perf stat` cache misses with vs. without zero-copy.

### PR7 — Tensor / Pose / Transform / Multi*

- Remaining standard catalogue.
- Optional `/tf` broadcast on `TransformMessage` and `PoseMessage` (shared `tf2_ros::TransformBroadcaster` per component).

### PR8 — Intra-process composability test

- `composable_container.launch.py` loads both components into one container with `use_intra_process_comms=True`.
- Integration test: a third `ComposableNode` (a simple test consumer in C++) subscribes to a bridge-published `sensor_msgs::Image` and asserts the pointer received in the callback equals the pointer published by the bridge — proving the intra-process zero-copy path.

### PR9 — Loaned-message path (advanced)

- Detect `can_loan_messages()` at runtime per publisher.
- For `Image`, `PointCloud2`, and large `Float32MultiArray`, when loaning is available, copy from the ZMQ frame into the loaned buffer and `publish(std::move(loaned))`.
- Document the requirement: `RMW_IMPLEMENTATION=rmw_cyclonedds_cpp` + Iceoryx, or `rmw_iceoryx_cpp`.

### PR10 — Docs & examples

- Update [docs/TODO](../docs/TODO).
- Add `docs/guides/ros2-bridge.md` walking through the YAML, the launch file, and a working demo.
- Add `examples/ros2_bridge/` with a `publisher.py` (cortex side) and a `ros2 topic echo` recipe.

---

## 10. Threading & lifecycle

- `CortexToRos2Bridge`: one `std::jthread` per Cortex SUB socket (small N) or one shared `zmq::poller_t` thread (large N). Threads exit when a `std::stop_token` is requested in the destructor.
- `Ros2ToCortexBridge`: subscription callbacks run on the rclcpp executor. Each Cortex PUB socket is owned by a single subscription's callback group so it is only ever published from one thread (PUB sockets are not thread-safe).
- The container is `component_container_mt`. Its `MultiThreadedExecutor` dispatches subscription callbacks across a small thread pool, but each callback group is serialized — the per-socket invariant holds.
- ZMQ context is shared across all sockets in one component. Context termination on destruction waits on `LINGER` (set to 0 to avoid teardown hangs).
- The discovery daemon is **not** managed by the bridge. The bridge logs a clear error and refuses to start any bridge entry whose topic cannot be resolved within a configurable timeout.

---

## 11. Testing & CI

- gtest on the config loader (PR1), wire decoder (PR2), and each adapter (PR3+).
- `launch_testing` integration tests that spin up `cortex.discovery.daemon` (Python) + a Python publisher/subscriber as helper processes, launch the bridge container, and assert end-to-end delivery. Fixtures live in `test/fixtures/`.
- Intra-process delivery test (PR8) using a small C++ consumer component.
- A staleness check in CI: re-run `gen_fingerprint_table.py` against the installed cortex package and `diff` against the checked-in header. Mismatch fails CI.
- Throughput benchmark for images and point clouds (PR6).
- CI matrix: ROS 2 Humble + Jazzy + Rolling on Ubuntu 22.04 + 24.04.

---

## 12. Open questions

1. **`DictMessage` / `ListMessage` interop.** JSON-encoded `std_msgs/String` is gross but unblocks users. Better long-term: encourage users to define a proper ROS message and a custom Cortex `Message` subclass, then write an adapter. Decide whether the JSON fallback ships at all in v1, or whether we refuse and fail loudly.
2. **Custom-type extension API.** v1 only covers the standard catalogue. The cleanest extension path is a separate ament_cmake package that depends on `cortex_ros2_bridge`, defines its own adapter, and calls `REGISTER_ADAPTER` in a plugin loaded via `pluginlib`. Decide whether to wire pluginlib in v1 or defer.
3. **Configurable adapter overrides in YAML.** e.g. `adapter: "my_pkg::BayerImageAdapter"` for non-default encodings. Trivial once pluginlib is in.
4. **TF integration scope.** `broadcast_tf: true` on `TransformMessage`/`PoseMessage`. Decide: one shared `tf2_ros::TransformBroadcaster` per component (cheaper, simpler) vs. one per bridge entry (more isolated). Default to shared.
5. **Sync vs. async Cortex mode.** Since the bridge is pure C++, the Cortex async-vs-sync distinction goes away — we always use raw `zmq::socket_t` with our own poller. This is closer to Cortex's sync subscriber path (see [docs/plans/sync-subscriber-latency.md](../docs/plans/sync-subscriber-latency.md)) and should hit sub-100 µs add-on latency on top of the underlying transport. The YAML `cortex_mode` field is therefore likely unnecessary — remove it before v1 freezes.
6. **Discovery client port.** The discovery REQ/REP protocol must be reimplemented in C++. It's small but it's a maintenance surface — confirm the protocol is stable (see [src/cortex/discovery/protocol.py](../src/cortex/discovery/protocol.py)) before committing. *Resolved in PR2:* ported as-is; one fragility flagged in §14.

---

## 13. Implementation findings (PR1 → PR2)

What we learned while writing PR1 and PR2 that should reshape the later PRs.

### 13.1 PR1 (config) — small surprises, mostly clean

- The schema needed one validation rule that wasn't in §5: `ros2_to_cortex` entries must specify `ros2.type` explicitly. The adapter can default the ROS type for the **forward** direction (`ImageMessage` → `sensor_msgs/Image`), but the reverse is ambiguous (`Twist` and `PoseStamped` both target `PoseMessage`). The loader rejects ambiguous reverse entries up front instead of letting them blow up at adapter resolution.
- The ament style lint suite (copyright, cpplint, uncrustify, pep257, flake8) is too noisy and self-contradictory (cpplint expects test filenames it cannot infer from our naming). We disabled all five in `CMakeLists.txt` and rely on cppcheck + lint_cmake + xmllint as the substantive linters. If we want enforced style, add a separate clang-format/black config — do *not* re-enable ament's defaults.

### 13.2 PR2 (wire library) — the load-bearing finding

**The "zero-copy" story is half-true.** The plan's §6.1 sketch implied a `ZmqBackedVector` that gives both pointer aliasing *and* content aliasing. PR2 proved you can have only one of those when the downstream type is `std::vector<uint8_t>` (which it is, for every `sensor_msgs` data field):

- **Pointer aliasing** ✓ — `ZmqAllocator<T>` returns the frame's buffer from `allocate()`, so the vector does not call `malloc`.
- **Content aliasing** ✗ — `std::vector(n, alloc)` value-initialises every element, overwriting the frame's bytes with `T()`. Any other sizing path has the same problem.
- **Lifetime piggybacking** ✓ — the shared_ptr inside the allocator keeps the frame alive for the vector's lifetime, even across moves. This is the genuine win.

**Practical implication for PR4–PR6**: the Image / PointCloud adapters' cortex→ROS path does `std::vector<uint8_t, ZmqAllocator<uint8_t>> data(N, alloc); std::memcpy(data.data(), frame->data(), N);` — one memcpy, zero `malloc`s. The PR8 (loaned-message) path becomes more important than originally framed: it is the *only* way to fully avoid the memcpy on default-allocator messages without a custom message type. Promote PR8 in priority or accept the memcpy as the steady-state cost.

The honest version of this trade-off is now documented at the top of [oob_buffer.hpp](cortex_ros2_bridge/include/cortex_ros2_bridge/cortex_wire/oob_buffer.hpp).

**Read-only aliasing is still free.** `OobBuffer<T>` (no `std::vector` involvement) gives true zero-copy *read* access to the frame. The decoder paths in adapters that don't have to fit into a `std::vector` (most internal computations, all logging/metrics) should use `OobBuffer` directly.

**msgpack-cxx in the public API was the right call but cost some CMake gymnastics.** `metadata.hpp` exposes `const msgpack::object &` so adapters get unfiltered access to nested dicts/lists without re-decoding. Ubuntu 22.04 ships msgpack-cxx headers via `libmsgpack-dev` but with **no CMake config**. We resolved this with `find_path(MSGPACK_INCLUDE_DIR msgpack.hpp)` and exporting the include directory rather than an imported target — the latter would have forced msgpack-cxx into our package's export set, which CMake forbids without an installed target. Pin this pattern in PR3 when the adapter base header also pulls in msgpack types.

**Discovery protocol has one fragile quirk.** Cortex packs `TopicInfo` as a *msgpack `bin` blob inside another msgpack map*, not as a nested map. The C++ encoder/decoder must match this byte-for-byte: see `pack_topic_info` / `unpack_topic_info` in [discovery_client.cpp](cortex_ros2_bridge/src/cortex_wire/discovery_client.cpp). If Cortex ever changes this on the Python side without a wire-version bump, the C++ bridge will silently misdecode. Worth coordinating with the upstream protocol — possibly add a `protocol_version` field in a future Cortex release.

**Fingerprints are tied to module path.** `compute_fingerprint` uses `f"{module}.{qualname}"`, so every standard message's fingerprint is hashed against `cortex.messages.standard.X`. Moving `cortex.messages.standard` to a different module path silently invalidates all bridges in the field. The C++ table records the qualified name and we test against it (see [test_fingerprint_table.cpp](cortex_ros2_bridge/test/test_fingerprint_table.cpp)) — a CI staleness check catches this, but the upstream package layout is now a wire-stability concern. Note in Cortex's `CHANGELOG` policy.

### 13.3 Gaps PR2 did not cover

These are real and need to land before PR3 can be finished cleanly:

- **Metadata encoder.** PR2 has `DecodedMetadata::from_bytes(...)` but no `serialize_metadata(...)` counterpart. The ros2→cortex direction (PR5) needs to *pack* a msgpack array of field values plus OOB descriptors. Add this in PR3 as part of the adapter base, since adapters need it in both directions.
- **Cortex `bytes`-typed fields.** msgpack distinguishes `STR` (UTF-8) from `BIN` (opaque bytes). `BytesMessage`'s `data` round-trips through Python as `BIN`. The decoder treats them correctly already; the encoder needs the same care. Add a test fixture for `BytesMessage` in PR3.
- **Nested dict / list walking.** `DictMessage` and `ListMessage` contain arbitrary nested structures that may themselves contain OOB descriptors (a dict-of-arrays is common). PR2 only exercises top-level OOB descriptors. Adapter code in PR3 must recurse on msgpack maps/arrays and dispatch to `as_oob` at every level. The unit-test suite needs a `dict_with_nested_array` case to lock this in.

### 13.4 Operational notes

- The neurosim:ros docker is the dev environment. Quirks:
  - PATH must put `/usr/bin` ahead of `/opt/conda/bin` so `catkin_pkg` from system Python is used. Required: `export PATH=/usr/bin:$PATH; unset PYTHONPATH`.
  - colcon build requires `-DPython3_EXECUTABLE=/usr/bin/python3` for the same reason.
  - `libmsgpack-dev` is not preinstalled; `apt-get update && apt-get install -y libmsgpack-dev` each run. Persist this in a derived Dockerfile to avoid the per-run install in CI.
  - colcon test loses `build/` between transient docker invocations. Build + test in one container run, or mount a persistent build volume.

---

## 14. Done definition

- `colcon build` builds the package on a clean ROS 2 Humble install.
- `ros2 launch cortex_ros2_bridge composable_container.launch.py config:=config/example_full.yaml` brings up the bridge, both directions, and `ros2 topic list` shows all configured topics.
- The example YAML round-trips every standard message type from a Python publisher → ROS 2 topic → another Python subscriber via the reverse bridge, with the original Cortex header preserved or reconstructed faithfully.
- For `ImageMessage`, profiling confirms no array copy on the Cortex→ROS 2 path (verified by memory address equality on the OOB buffer through the adapter).
- Integration test demonstrates that a co-loaded composable consumer of `sensor_msgs/Image` receives the message via intra-process delivery (no DDS roundtrip).
- Docs page exists at `docs/guides/ros2-bridge.md`.
