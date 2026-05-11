# cortex_ros2_bridge

A ROS 2 package that bridges Cortex pub/sub topics to ROS 2 topics, in both directions, configured by a single YAML file.

The bridge ships as two composable rclcpp nodes — one per direction — that can be loaded into a `component_container_mt` together with downstream ROS 2 consumers for **intra-process zero-copy delivery**. They also run as plain executables for development.

```
┌───────────────────────────┐                ┌─────────────────────────────┐
│ Cortex publisher  (Py)    │                │ rclcpp consumer  (C++)      │
│ /sensor/camera/rgb        │                │ /camera/image_raw           │
└──────────┬────────────────┘                └─────────────┬───────────────┘
           │                                               ▲
           │ ZMQ multipart over ipc://                     │ rclcpp intra-process
           │ [topic, header, metadata, *oob_frames]        │ or DDS
           ▼                                               │
┌─────────────────────────────────────────────────────────────────────────┐
│                         ComposableNodeContainer                         │
│   ┌────────────────────────┐         ┌────────────────────────────┐     │
│   │ CortexToRos2Bridge     │         │ Ros2ToCortexBridge         │     │
│   │ - SUB socket per topic │         │ - PUB socket per topic     │     │
│   │ - adapter -> ROS 2 msg │         │ - rclcpp sub -> adapter    │     │
│   └────────────────────────┘         └────────────────────────────┘     │
└──────────┬─────────────────────────────────────────────────────┬────────┘
           │   discovery REQ/REP  (ipc:///tmp/cortex_discovery)  │
           ▼                                                     ▼
                       ┌───────────────────────┐
                       │  cortex-discovery     │
                       │  (Python daemon)      │
                       └───────────────────────┘
```

## Prerequisites

- ROS 2 Humble or later (ament_cmake, rclcpp, rclcpp_components, sensor_msgs, geometry_msgs, std_msgs, builtin_interfaces).
- A C++17 compiler.
- System packages: `libzmq3-dev`, `libmsgpack-dev`, `libyaml-cpp-dev`, `libcppzmq-dev`.
- [`cortex_wire_cpp`](../../cortex_wire_cpp) — a pure-CMake sibling library that this package depends on. If it isn't installed, our `CMakeLists.txt` falls back to `add_subdirectory` against the in-tree source at `../../cortex_wire_cpp`.
- A running Cortex discovery daemon (`cortex-discovery` from the [Python cortex package](../../)).

## Build

From a colcon workspace that contains this package under `src/`:

```bash
colcon build --packages-select cortex_ros2_bridge
source install/setup.bash
```

On Anaconda-based ROS containers (where the default `python3` lacks `catkin_pkg`), point CMake at the system Python:

```bash
colcon build --packages-select cortex_ros2_bridge \
  --cmake-args -DPython3_EXECUTABLE=/usr/bin/python3
```

The package also includes the test suite:

```bash
colcon test --packages-select cortex_ros2_bridge
colcon test-result --verbose
```

## Configuration

Every bridged topic is one entry in a YAML file. The complete schema:

```yaml
version: 1                                   # required, currently 1

cortex:
  discovery_address: "ipc:///tmp/cortex_discovery"   # daemon REQ/REP endpoint
  node_name_prefix:  "cortex_bridge"                 # used to derive PUB endpoints

defaults:                                    # applied to entries that omit qos
  qos:
    reliability: reliable                    # reliable | best_effort
    durability:  volatile                    # volatile | transient_local
    history:     keep_last                   # keep_last | keep_all
    depth:       10
  loaned_messages: auto                      # auto | force | never (reserved for future use)

# Cortex publishers -> ROS 2 subscribers
cortex_to_ros2:
  - name: camera
    cortex:
      topic: "/sensor/camera/rgb"
      type:  ImageMessage                    # name from cortex.messages.standard
    ros2:
      topic: "/camera/image_raw"
      type:  "sensor_msgs/msg/Image"         # explicit; default-inference is deferred
      frame_id: "camera_optical"             # optional; overrides any wire frame_id
    qos:
      reliability: best_effort
      depth: 5

# ROS 2 publishers -> Cortex subscribers
ros2_to_cortex:
  - name: cmd
    ros2:
      topic: "/cmd_string"
      type:  "std_msgs/msg/String"
    cortex:
      topic: "/control/cmd"
      type:  StringMessage
    qos:
      reliability: reliable
      depth: 1
```

Validation rules:

- `version` must be `1`.
- Each entry `name` is unique across both directions.
- `ros2.type` is **required** for `ros2_to_cortex` entries (the mapping ROS→Cortex is ambiguous; we won't guess). It is also required for `cortex_to_ros2` entries today, although default-by-Cortex-kind lookup is on the roadmap.
- Unknown `cortex.type` names, missing adapters, and discovery fingerprint mismatches fail the affected entry loudly and the bridge keeps the others alive.

Examples ship in [config/example_minimal.yaml](config/example_minimal.yaml) and [config/example_full.yaml](config/example_full.yaml).

## Launch

Three launch files are installed under `share/cortex_ros2_bridge/launch/`:

| File | Purpose |
| --- | --- |
| `cortex_to_ros2.launch.py`     | One container, one component: Cortex → ROS 2 only. |
| `ros2_to_cortex.launch.py`     | One container, one component: ROS 2 → Cortex only. |
| `composable_container.launch.py` | One container, both components. **Use this for production** so downstream ROS 2 consumers can compose into the same container and receive bridge-published messages via intra-process delivery. |

Run a bridge:

```bash
# discovery daemon (separate terminal)
cortex-discovery

# both directions, both composable nodes in one container with
# use_intra_process_comms enabled
ros2 launch cortex_ros2_bridge composable_container.launch.py \
  config:=/path/to/bridge.yaml
```

The launch argument `container_name` is exposed if you want to load additional composable nodes into the same container later (`container_name:=cortex_bridge_container` is the default).

For development without a container (no intra-process zero-copy, easier to attach a debugger):

```bash
ros2 run cortex_ros2_bridge cortex_to_ros2 \
  --ros-args -p config_path:=/path/to/bridge.yaml

ros2 run cortex_ros2_bridge ros2_to_cortex \
  --ros-args -p config_path:=/path/to/bridge.yaml
```

## Supported message types

| Cortex message | ROS 2 message | Notes |
| --- | --- | --- |
| `StringMessage` | `std_msgs/msg/String` | |
| `IntMessage` | `std_msgs/msg/Int64` | |
| `FloatMessage` | `std_msgs/msg/Float64` | |
| `BytesMessage` | `std_msgs/msg/ByteMultiArray` | Cortex side is msgpack `BIN`, not `STR`. |
| `TimestampMessage` | `builtin_interfaces/msg/Time` | |
| `HeaderMessage` | `std_msgs/msg/Header` | Sequence field is dropped (no equivalent in ROS 2's stripped Header). |
| `ArrayMessage` | `std_msgs/msg/Float32MultiArray` | Strict dtype `<f4`. Layout dims carry shape. |
| `ArrayMessage` | `std_msgs/msg/Float64MultiArray` | Strict dtype `<f8`. |
| `ImageMessage` | `sensor_msgs/msg/Image` | One memcpy from the ZMQ OOB frame to `Image::data` (see *Zero-copy* below). |
| `PointCloudMessage` | `sensor_msgs/msg/PointCloud2` | xyz always; rgb/intensity/normals round-trip when present, msgpack `nil` when absent. |
| `PoseMessage` | `geometry_msgs/msg/PoseStamped` | `child_frame_id` is dropped (PoseStamped has none). |
| `TransformMessage` | `geometry_msgs/msg/TransformStamped` | 4×4 ↔ translation + quaternion. `broadcast_tf` YAML field is currently unwired. |
| `TensorMessage` | `std_msgs/msg/Float32MultiArray` | Drops torch device / requires_grad on the forward path; emits CPU on reverse. |

Not yet supported (deferred): `MultiArrayMessage`, `MultiTensorMessage` (need per-key split), `DictMessage`/`ListMessage` (lossy JSON path under design).

## Zero-copy notes

The intra-process composability path delivers `std::unique_ptr<Msg>` from the bridge directly to a colocated subscription without DDS serialisation, when:

1. Both the bridge component and the subscriber are loaded into the **same** `component_container_mt`.
2. Both use `use_intra_process_comms: true` (the launch files set this).
3. The subscriber takes the message via `std::unique_ptr<const Msg>` or `std::shared_ptr<const Msg>`.

On the ZMQ ingress side, the OOB frames that carry arrays are not memcpy'd until they hit the adapter; the adapter's `to_ros2` is where data ends up in the published ROS message. For `sensor_msgs/Image`, that adapter performs **one memcpy** from the ZMQ frame into a freshly-allocated `Image::data`. Eliminating that memcpy requires either a loaned-message-capable RMW (Iceoryx) or a custom intra-process message type, both of which are out of scope for v1.

## Discovery daemon

The bridge does not manage the daemon. Bring it up out of band:

```bash
cortex-discovery                                # default endpoint
cortex-discovery --address ipc:///tmp/my_disc   # custom endpoint, also set in YAML
```

`Ros2ToCortexBridge` registers itself with the daemon at startup and unregisters on shutdown. `CortexToRos2Bridge` looks up its source topics' endpoints once at startup; topics that aren't yet registered cause that entry to be skipped (other entries keep running).

## Troubleshooting

| Symptom | Cause |
| --- | --- |
| `discovery: topic '/foo' not registered — skipping` | The Cortex publisher hasn't started yet, or `discovery_address` in YAML doesn't match the daemon. |
| `fingerprint mismatch: daemon=0x…, expected=0x… — refusing to bridge` | The publisher is running a Cortex `messages.standard` version different from the one this bridge was built against. Rebuild after regenerating `fingerprint_table.hpp` (see *Development* below). |
| `cannot create parent dir for ipc:///tmp/...` | Filesystem permissions on `/tmp/cortex/topics/` are wrong. |
| `unknown cortex type 'XMessage' — skipping` | YAML references a Cortex message we don't have an adapter for. |
| `factory returned null binding` | An adapter is registered for the cortex kind, but not for the requested `ros2.type` combination. Check the supported types table above. |

## Development

The package layout:

```
cortex_ros2_bridge/
├── include/cortex_ros2_bridge/
│   ├── config.hpp                          # YAML schema + loader
│   ├── qos.hpp                             # QosSettings -> rclcpp::QoS
│   ├── adapter.hpp                         # adapter ABCs + Inbound/Outbound
│   ├── registry.hpp                        # type-erased adapter registry
│   ├── binding.hpp                         # templated per-topic plumbing
│   ├── binding_factory.hpp                 # string -> binding factory
│   ├── cortex_to_ros2_node.hpp             # composable component
│   ├── ros2_to_cortex_node.hpp             # composable component
│   └── adapters/                           # one header per Cortex message
├── src/                                    # matching implementations
├── launch/                                 # 3 launch files
├── config/                                 # example YAMLs
└── test/                                   # gtest suite
```

The Cortex wire format library lives separately at [`cortex_wire_cpp/`](../../cortex_wire_cpp) as a pure-CMake project (no ROS dependency). It is consumed via `find_package(cortex_wire_cpp)`; the bridge's CMakeLists falls back to `add_subdirectory` for monorepo development.

The C++ side fingerprint table (`cortex_wire_cpp/include/cortex_wire/fingerprint_table.hpp`) is generated from the Python `cortex.messages.standard` catalogue. Regenerate after any change to that module:

```bash
python3 -m venv /tmp/cortex_venv
/tmp/cortex_venv/bin/pip install -e /path/to/cortex
/tmp/cortex_venv/bin/python ../../cortex_wire_cpp/scripts/gen_fingerprint_table.py
```

Pass `--check` in CI to verify the committed header matches the installed cortex package.

For a full code walkthrough of how everything fits together, see [STUDY_GUIDE.md](../STUDY_GUIDE.md).

## License

Apache-2.0. See [../../LICENSE](../../LICENSE).
