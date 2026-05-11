# cortex_wire_cpp

C++ port of Cortex's IPC wire format. Standalone CMake library, no ROS 2 dependency.

Used by the ROS 2 bridge ([ros2_bridge/cortex_ros2_bridge](../ros2_bridge/cortex_ros2_bridge)) but designed for any C++ consumer that wants to read or write Cortex's pub/sub IPC format: debug tools (LCM-spy style), other language bridges, or sub-100µs control-loop consumers that don't want to go through Python.

## What it provides

- `cortex_wire::MessageHeader` — 24-byte big-endian fingerprint/timestamp/sequence header.
- `cortex_wire::DecodedMetadata` — msgpack-cxx decoder for the metadata frame, with `OobDescriptor` parsing for numpy/torch out-of-band buffers.
- `cortex_wire::OobBuffer<T>` — zero-copy read-only view into a ZMQ frame.
- `cortex_wire::ZmqAllocator<T>` — std-allocator that backs a `std::vector` with a ZMQ frame's buffer (zero allocation; not zero-copy for content — see header for caveats).
- `cortex_wire::DiscoveryClient` — sync REQ/REP client for the discovery daemon.
- `cortex_wire::fingerprint_table` — generated map from Cortex's standard catalogue.

Layout:

```
cortex_wire_cpp/
├── CMakeLists.txt
├── include/cortex_wire/
│   ├── header.hpp
│   ├── metadata.hpp
│   ├── oob_buffer.hpp
│   ├── discovery_client.hpp
│   └── fingerprint_table.hpp        ← auto-generated
├── src/
│   ├── header.cpp
│   ├── metadata.cpp
│   └── discovery_client.cpp
├── scripts/
│   └── gen_fingerprint_table.py     ← regen tool
├── cmake/
│   └── cortex_wire_cppConfig.cmake.in
└── test/                            ← 5 gtest binaries
```

## Dependencies

- `cppzmq` — header-only C++ wrapper over `libzmq` (system).
- `msgpack-cxx` — header-only msgpack C++ library. On Ubuntu 22.04 install `libmsgpack-dev`. Newer distros provide a CMake config under `msgpack-cxx` which is picked up automatically.
- C++17.

## Build & install

Pure CMake, no colcon:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
sudo cmake --install build
```

## Consuming from another CMake project

```cmake
find_package(cortex_wire_cpp REQUIRED)

add_executable(my_tool my_tool.cpp)
target_link_libraries(my_tool PRIVATE cortex_wire::cortex_wire)
```

For monorepo consumers (e.g. `ros2_bridge`) the same target is available via `add_subdirectory(../../cortex_wire_cpp ...)` — no install step needed for dev iteration.

## Regenerating the fingerprint table

`include/cortex_wire/fingerprint_table.hpp` is committed so the library builds without a Python `cortex` install. Regenerate after any change to `cortex.messages.standard`:

```bash
python3 -m venv /tmp/cortex_venv
/tmp/cortex_venv/bin/pip install -e /path/to/cortex
/tmp/cortex_venv/bin/python scripts/gen_fingerprint_table.py
```

`--check` mode for CI:

```bash
scripts/gen_fingerprint_table.py --check
```
