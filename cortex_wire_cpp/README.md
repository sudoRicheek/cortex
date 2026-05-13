# cortex_wire_cpp

C++ implementation of the Cortex IPC stack: wire codec, discovery client, and
a small pub/sub client. Standalone pure-CMake library, no ROS 2 dependency.

Used by the ROS 2 bridge
([`ros2_bridge/cortex_ros2_bridge`](../ros2_bridge/cortex_ros2_bridge)) but
designed for any C++ consumer that needs to read or write Cortex's pub/sub
IPC format — debug tools, alternative-language bridges, sub-100µs control
loops that don't want to go through Python.

## What's inside

| Layer        | Components                                                              |
|--------------|-------------------------------------------------------------------------|
| Codec        | `MessageHeader`, `DecodedMetadata`, `MetadataBuilder`, `OobBuffer<T>`   |
| Discovery    | `DiscoveryClient` (REQ/REP, sync)                                       |
| Pub/Sub      | `Context`, `Publisher`, `Subscriber`                                    |
| Type catalog | `fingerprint_table` (generated from Python `cortex.messages.standard`)  |

See **[DOCS.md](DOCS.md)** for the full API surface, Python feature-parity
table, OOB-scope explainer, and examples.

## Layout

```
cortex_wire_cpp/
├── CMakeLists.txt
├── DOCS.md
├── include/cortex_wire/
│   ├── header.hpp
│   ├── metadata.hpp
│   ├── oob_buffer.hpp
│   ├── discovery_client.hpp
│   ├── context.hpp                    ← shared ZMQ context handle
│   ├── publisher.hpp                  ← PUB + discovery register
│   ├── subscriber.hpp                 ← SUB recv thread + fingerprint check
│   └── fingerprint_table.hpp          ← auto-generated
├── src/
│   ├── header.cpp
│   ├── metadata.cpp
│   ├── discovery_client.cpp
│   ├── publisher.cpp
│   └── subscriber.cpp
├── scripts/
│   └── gen_fingerprint_table.py       ← regen tool
├── cmake/
│   └── cortex_wire_cppConfig.cmake.in
└── test/                              ← 6 gtest binaries
```

## Dependencies

- `cppzmq` — header-only C++ wrapper over `libzmq` (system).
- `msgpack-cxx` — header-only msgpack C++ library. On Ubuntu 22.04 install
  `libmsgpack-dev`. Newer distros provide a CMake config under `msgpack-cxx`
  which is picked up automatically.
- POSIX threads (linked publicly; the pub/sub client owns a recv thread).
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

For monorepo consumers (e.g. `ros2_bridge`) the same target is available via
`add_subdirectory(../../cortex_wire_cpp ...)` — no install step needed for
dev iteration.

## Regenerating the fingerprint table

`include/cortex_wire/fingerprint_table.hpp` is committed so the library
builds without a Python `cortex` install. Regenerate after any change to
`cortex.messages.standard`:

```bash
python3 -m venv /tmp/cortex_venv
/tmp/cortex_venv/bin/pip install -e /path/to/cortex
/tmp/cortex_venv/bin/python scripts/gen_fingerprint_table.py
```

`--check` mode for CI:

```bash
scripts/gen_fingerprint_table.py --check
```
