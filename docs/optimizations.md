# Cortex Optimization Notes

This document covers the optimizations implemented in March 2026 to reduce serialization and transport overhead in Cortex without introducing shared memory.

## Goals

- Reduce Python-side copying for large NumPy payloads.
- Reduce per-message metadata overhead.
- Preserve the public `Message`, `Publisher`, and `Subscriber` APIs.
- Keep the discovery plane unchanged.

## Implemented Changes

### 1. Frame-aware transport for large payloads

Before this change, a message was serialized into a single Python `bytes` blob and sent as one ZMQ frame. For array-heavy robotics traffic, that meant:

- building a large Python bytes object in user space,
- copying array contents into that bytes object,
- then handing the merged blob to ZeroMQ for another copy into its own buffers.

The transport path now uses message frames:

- frame 1: fixed-size message header,
- frame 2: packed metadata for field values,
- frame 3+: raw array or tensor buffers.

This matters because top-level and nested arrays no longer need to be flattened into the metadata blob. The sender can hand ZeroMQ a view of the contiguous array buffer directly.

### 2. Schema-ordered message payloads

`Message.to_bytes()` previously serialized message fields as a dictionary keyed by field name. That repeated field names on every message and forced more structure building on both encode and decode.

Messages now serialize field values in dataclass declaration order. The receiver reconstructs the message using the cached field order for that message class.

Benefits:

- less metadata per message,
- fewer Python objects created on the hot path,
- no repeated field-name encoding on every publish.

### 3. Cached message schema metadata

Message classes now cache their ordered field names. This is a small optimization by itself, but it matters because the send path is otherwise rebuilding dataclass metadata every time.

### 4. Msgpack extension types for inline array/tensor support

The generic serialization helpers no longer recursively wrap every nested value with custom type headers. Instead, they lean on msgpack directly and use extension hooks for:

- NumPy arrays,
- PyTorch tensors.

That removes a large amount of Python recursion and custom length-prefix bookkeeping for nested dict/list payloads.

### 5. Zero-copy NumPy decode by default

`deserialize_numpy()` used to end with `arr.copy()`, which guaranteed an extra full-memory copy on every decode.

That copy is now removed by default. The function returns a NumPy array backed by the source buffer unless the caller explicitly asks for `copy=True`.

This is a meaningful win for image, lidar, and tensor-like workloads where the extra copy was pure overhead.

### 6. Subscriber receive path now uses non-copying ZMQ frame access

The subscriber now receives multipart data with `copy=False` and reconstructs arrays from the underlying frame buffers. This lines up with the out-of-band transport and prevents unnecessary Python-side copies before NumPy even sees the data.

## Expected Impact

The main expected wins are:

- lower CPU time for large array messages,
- lower allocator pressure,
- lower tail latency under sustained throughput,
- better bandwidth utilization for image and point-cloud payloads.

The biggest gains should show up on:

- `ArrayMessage`,
- `ImageMessage`,
- point cloud style messages with multiple arrays,
- nested dictionary payloads that include arrays.

The smallest gains will be on tiny primitive-only messages, where ZeroMQ scheduling and Python coroutine overhead dominate.

## Tradeoffs Introduced

- Some decoded NumPy arrays now reference the incoming transport buffer instead of owning their own copy. That is faster, but it means downstream code should copy only when it truly needs ownership or mutability guarantees.
- The frame transport path is more complex than the original single-blob path. That complexity is justified for robotics-sized payloads, but it does raise maintenance cost.

## What Was Deliberately Not Done

- No shared memory transport.
- No compression.
- No alternate wire format such as FlatBuffers, Cap'n Proto, or Protobuf.
- No change to the discovery architecture.
- No batching layer above PUB/SUB.

## Recommended Next Steps

If more performance is needed, the next high-value steps are:

1. Add benchmark coverage that measures `to_bytes()` versus `to_frames()` by payload type and size.
2. Split control-plane and data-plane tuning explicitly, including socket options such as HWM, `IMMEDIATE`, and publish/drop policy.
3. Introduce explicit ownership semantics for received arrays so users can choose `borrowed` versus `owned` decode behavior.
4. Revisit the message format for multi-array messages to avoid repeated per-array metadata when schema is fixed.
5. Only after those steps, consider shared memory for very large colocated pipelines.