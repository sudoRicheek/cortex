# Cortex Critique

A bottom-up review of Cortex as it stands today, with a focus on its viability as a communication library for robotics. This complements [design-review.md](design-review.md) with concrete code-level findings and benchmark observations.

## How Cortex works (bottom-up)

### 1. Fingerprinting — `utils/hashing.py`

A message class's identity is a 64-bit integer:

```
fingerprint = SHA-256(f"{module}.{qualname}|{','.join(sorted('field:type'))}")[:8]
```

- Computed lazily and cached in `_fingerprint_cache`.
- `field.type` is a string when `from __future__ import annotations` is active and a real type otherwise. The fingerprint therefore depends on how the module was imported — fragile for cross-repo use.
- Field ordering is sorted alphabetically in the fingerprint, but the wire layout uses dataclass declaration order. Two classes could theoretically fingerprint identically but interpret the wire differently.

### 2. Message base — `messages/base.py`

Each dataclass inheriting `Message` is auto-registered via `__init_subclass__` into `MessageType._registry[fingerprint] = cls`.

Wire format (multipart transport, what publishers actually use):

```
Frame 0: topic bytes            (for PUB/SUB filter)
Frame 1: 24-byte header         (fingerprint u64, timestamp_ns u64, sequence u64, big-endian)
Frame 2: msgpack of ordered field values with OOB descriptors
Frame 3..N: raw contiguous array buffers (zero-copy)
```

There is a second, legacy single-blob path (`to_bytes` / `from_bytes`) that embeds array bytes inside a single msgpack blob using ExtType. It is retained for `Message.decode(...)` and tests, but is not what the transport uses.

### 3. Serialization — `utils/serialization.py`

Two strategies coexist:

- `_msgpack_default` / `_msgpack_ext_hook` (inline): arrays/tensors get packed as msgpack ExtType inside the single blob. Used by the legacy path.
- `_encode_transport_value` / `_decode_transport_value` (out-of-band): each array/tensor is replaced with a tiny dict `{__cortex_oob__: "numpy", buffer: i, dtype, shape}` and its raw bytes are appended as separate ZMQ frames. Reconstruction uses `np.frombuffer(frame.buffer, dtype).reshape(shape)` with no copy.

After the March 2026 optimizations: zero-copy decode, schema-ordered values (field names no longer repeated per message), and cached field-name tuples.

### 4. Discovery — `discovery/daemon.py` and `discovery/client.py`

Single-threaded `zmq.REP` over IPC at `ipc:///tmp/cortex/discovery.sock`.

- Registry is a plain `dict[str, TopicInfo]`, enforcing one publisher per topic.
- RCVTIMEO=1s so the run loop can poll `_running` for Ctrl-C.
- Commands: REGISTER, UNREGISTER, LOOKUP, LIST, SHUTDOWN.
- Request/response payloads are msgpack.
- Client uses REQ with close-and-recreate on timeout (REQ sockets are stuck after a missed reply).

### 5. Publisher / Subscriber — `core/publisher.py`, `core/subscriber.py`

- **Publisher**: binds a `zmq.PUB` at `ipc:///tmp/cortex/topics/<node>__<topic>.sock`, registers via the discovery client, publishes multipart `[topic, header, metadata, *buffers]` with `zmq.NOBLOCK`. If the `Node` hands it an async context, it wraps a sync `zmq.Context(self._context)` around the same underlying zmq io threads so publishing stays synchronous.
- **Subscriber**: uses an async context, looks up the topic (optionally waits), connects `zmq.SUB`, sets a topic filter, loops via `AsyncExecutor` doing `recv_multipart(copy=False)` → `Message.from_frames`.

### 6. Node + Executors — `core/node.py`, `core/executor.py`

A `Node` owns a shared `zmq.asyncio.Context`, plus lists of publishers, subscribers, and timers. Each timer gets a `RateExecutor(fn, rate_hz)`. `node.run()` creates asyncio tasks for every timer and every callback-subscriber, then `asyncio.gather`. `RateExecutor` uses `perf_counter` plus `asyncio.sleep(max(0, next-now))`. `cortex.run` prefers uvloop on Unix.

## Benchmark results

Measured on this machine with the in-repo benchmark suite:

| Metric                    | Value                       |
| ------------------------- | --------------------------- |
| Small-payload latency     | mean 556 µs, p99 1075 µs    |
| 64KB latency              | mean 919 µs, p99 1.4 ms     |
| Tiny array throughput     | 21.8k msg/s                 |
| 1MB array throughput      | 7.7k msg/s, 8.0 GB/s        |
| 4MB array throughput      | 2.25k msg/s, 9.4 GB/s       |
| 1080p RGB frames          | 1422 fps, 8.8 GB/s          |
| Raw wire+decode (inproc)  | 35 µs roundtrip (4MB array) |

The delta between the **~35 µs raw wire** and **~550 µs end-to-end** is asyncio scheduling, context-switch between publisher timer and subscriber recv, and Python callback dispatch. Serialization is close to memcpy-bandwidth on large payloads — the OOB transport is pulling its weight.

## What can be improved

### Design-level (biggest wins)

1. **Latency floor is too high for control loops.** ~550 µs mean and ~1.5 ms p99 is dominated by `asyncio` + `zmq.asyncio`, not zmq itself. Control topics should be able to opt into a synchronous thread-plus-`zmq.Poller` receive path targeting <100 µs p99. Async should be the default, not the only option.

2. **Discovery is a single REQ/REP chokepoint with stop-the-world semantics.** On crashes, stale topic entries are never reclaimed — a crashed publisher's IPC file stays on disk and the registry keeps pointing at a dead socket. Add leases with heartbeats (publisher renews every N seconds; daemon evicts stale entries), or a peer-gossip model where every node beacons presence. The current daemon has no concurrency — one slow client blocks all others.

3. **One-publisher-per-topic is a hard limit for robotics.** Redundant IMUs, failover, and multi-source fusion are all blocked. The registry should accept N publishers per topic and subscribers should `connect()` to all of them — ZMQ SUB handles fan-in natively.

4. **No backpressure semantics.** `pub.publish()` is `NOBLOCK` and silently drops on HWM. Subscriber HWM=10 on SUB evicts old messages by default. Robotics needs per-topic QoS profiles similar to DDS:
   - `best_effort_latest` — camera frames: drop old, keep newest (`ZMQ_CONFLATE=1`).
   - `reliable_queue` — commands: block or surface an error.
   - `dropping_queue` — telemetry: current behavior, but with a drop counter.

5. **No liveness or drop detection.** A subscriber has no way to know the publisher died. Sequence numbers exist in the header but are never checked for gaps. Automatic gap-counting in Subscriber would be gold for debugging.

6. **Callback execution blocks the receive loop.** A 10 ms callback accumulates on SUB HWM and drops. Receive, decode, and user-callback execution should be decoupled with a bounded work queue and one or more worker coroutines/threads per subscriber. ROS 2 executors have this distinction for a reason.

7. **Local-only transport in practice.** Addresses are hardcoded `ipc://` paths under `/tmp`. Multi-host robotics (robot ↔ base-station) needs TCP transport in discovery, NIC selection, and topology-aware addressing.

8. **No shared memory for huge payloads.** At 9 GB/s on 4 MB arrays, every subscriber gets a fresh copy. For multi-subscriber camera or LiDAR fan-out, a shared-memory transport (posix shm + ring buffer + zmq for control-plane notifications) would give true zero-copy.

### Code-level issues

9. `publisher.py:91-95` — `zmq.Context(self._context)` creates a shadowed sync context sharing the async context's io threads. Correct, but subtle. `zmq.PUB` is **not thread-safe** — calling `pub.publish()` from multiple asyncio tasks on the same socket is undefined. Needs docs or a lock.

10. `publisher.py:117-118` — the publisher unlinks any existing socket file on startup. If two publishers on the same host use the same node name + topic, the second silently steals the socket. Should fail loudly.

11. `subscriber.py:155-160` — fingerprint mismatch logs a warning and proceeds anyway. That is a silent-data-corruption path. Should refuse to connect.

12. `messages/base.py:109-129` — `_sequence_counter` is **class-level**, shared across every Publisher instance of that message type in the process. Two publishers of `ArrayMessage` interleave sequences — breaking per-topic drop detection. Move it onto the `Publisher`.

13. `utils/hashing.py:34-38` — `field.type` is a string with PEP 563 and a real type otherwise; the resulting fingerprint differs across import environments. Use `typing.get_type_hints(cls)` consistently.

14. `discovery/client.py:78-101` — `retries=1` default means zero retries (loop runs once). Fencepost bug.

15. `core/executor.py:119-147` — `RateExecutor` has both `await asyncio.sleep(0)` inside the loop and `await asyncio.sleep(max(0, dt))` at the bottom. The first is redundant and creates unnecessary wakeups. Catch-up logic silently eats dropped ticks; control loops often need to know.

16. `discovery/daemon.py:87` — RCVTIMEO=1s means Ctrl-C takes up to 1s to take effect and request throughput is throttled. A `zmq.Poller` with a shutdown PAIR socket gives clean immediate shutdown.

17. `messages/standard.py:146-150` — `ImageMessage.__post_init__` auto-fill is non-idempotent across deserialization round-trips. Minor.

18. `discovery/daemon.py:168-177` — same-publisher re-registration is allowed; if its IPC path changed, existing subscribers are never told. Needs a lease or a "changed" notification.

19. **No CI test for cross-process fingerprint stability.** Given how much safety rides on fingerprints, every standard message type deserves a stored golden fingerprint asserted in CI.

20. **`from_bytes` vs `from_frames` asymmetry is a trap.** `Message.decode(bytes)` only handles the inline path. If anyone captures bytes from the wire (the multipart path) and calls `decode()`, it will fail silently. Unify the paths or rename `decode`.

21. **No async publish.** `send_multipart` briefly blocks on HWM/context switch; inside an async timer callback this is a hidden blocking call. An async `publish` variant would help.

### Schema evolution

22. No optional fields, no versioning. For long-lived robotics deployments, add:
    - field defaults (so fingerprints tolerate missing trailing fields on decode),
    - an `msg_schema_version: int = 1` convention,
    - eventually, a real wire schema (FlatBuffers, Cap'n Proto, or generated-from-.fbs dataclasses).

## Summary

Cortex is a well-built, honest small-system IPC library. The **serialization is genuinely fast** — hitting memcpy-bandwidth on 4 MB arrays with zero-copy OOB frames. The **latency floor (~550 µs p50, ~1.5 ms p99)** is limited by asyncio, not zmq. The **discovery, QoS, liveness, and single-host assumptions** are the real blockers for using this as robotics middleware.

Recommended path if adopting Cortex for robotics:

1. Add per-topic QoS profiles with drop counters (1-2 days).
2. Add a synchronous-threaded subscriber option for low-latency control (1 day).
3. Add heartbeats/leases and multi-publisher support to discovery (3-5 days).
4. Add TCP transport and host-aware discovery (2-3 days).
5. Then consider shared memory and schema evolution.
