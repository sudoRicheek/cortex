# Sync subscriber & publisher mode

`mode='sync'` opts a subscriber or publisher out of asyncio. Use it for control loops where p99 jitter — not throughput — is what matters.

## When to use which

| Use case | Mode | Why |
|---|---|---|
| Camera frames, point clouds, logs | `async` | Throughput-bound; latency floor irrelevant. |
| Dashboards, HTTP / WebSocket streaming | `async` | Composes with `asyncio` servers. |
| Closed-loop control at > 100 Hz | `sync` | Sub-100 µs p99 reachable; jitter matters. |
| Robot teleop commands | `sync` | Operator-perceived latency. |
| Heartbeats, registration | `async` | Once per second — not worth the complexity. |

If in doubt, start with `async`.

## Sync subscriber

```python
def on_cmd(msg, header):           # plain function, NOT async def
    apply_command(msg)

node.create_subscriber(
    topic_name="/cmd/wheel",
    message_type=WheelCommand,
    callback=on_cmd,
    mode="sync",
    queue_size=1,                  # latest-wins (default)
    cpu_affinity=[3],              # optional, Linux
    sched_priority=20,             # optional, requires CAP_SYS_NICE
)
```

Construction blocks on a discovery lookup (`TimeoutError` if the topic never registers). The receive thread starts when `node.run()` is called and joins cleanly on `node.close()`.

### Contracts

- **Synchronous callback only.** The receive loop runs on a dedicated OS thread; passing an `async def` raises `TypeError` at construction time.
- **Strict fingerprint check.** A topic/type mismatch raises `MessageFingerprintError` instead of logging a warning. Silent type confusion is unacceptable for control topics.
- **Latest-wins by default.** `queue_size=1` so the receiver drops old messages on overflow and always sees the freshest command. Override if backlog tolerance matters more than freshness.
- **Independent zmq context.** No shared IO threads with the asyncio context — a stuck callback can't back-pressure the rest of the node.

### Determinism knobs

- `cpu_affinity=[N, ...]` — pin the receive thread to specific CPUs (Linux). Most useful on busy systems where the kernel migrates the thread between cores.
- `sched_priority=N` — run under `SCHED_FIFO` at priority `N` (Linux, requires `CAP_SYS_NICE`). Reach for this when `cpu_affinity` alone isn't enough — typically when other RT-priority work shares the machine. Failure to set is logged; the thread keeps running on the default scheduler.

Both are no-ops on platforms that don't support them.

## Sync publisher

The default publisher shares the node's `zmq.asyncio.Context` (with a sync shadow). For publishers driven from a non-asyncio thread — a tight C-extension loop, or the body of a sync subscriber callback — pass `mode='sync'`:

```python
pub = node.create_publisher(
    topic_name="/cmd/wheel",
    message_type=WheelCommand,
    mode="sync",
)
```

Gives the publisher its own independent `zmq.Context`. `publish()` becomes a direct syscall on the calling thread, no asyncio IO threads in the path. The `Node` tracks the extra context and terms it on close.

!!! danger "Still not thread-safe"
    `zmq.PUB` is not safe across threads. Only call `publish()` from one thread per Publisher.

## Free-threaded CPython

The sync floor is what you get from raw zmq + `zmq.Poller`. To clear it further when sharing a process with a busy asyncio loop, run on `python3.14t` (free-threaded build, PEP 779) with the GIL actually disabled:

```bash
PYTHON_GIL=0 python3.14t your_node.py
```

`msgpack._cmsgpack` re-enables the GIL on free-threaded builds, so `PYTHON_GIL=0` is the documented override. Cortex emits a one-line runtime hint when the configuration is suboptimal.

## Tracing the budget

Set `CORTEX_TRACE_LATENCY=N` to record up to N per-stage samples (recv, decode, callback) in ns:

```python
import os
os.environ["CORTEX_TRACE_LATENCY"] = "10000"

# ... run workload ...

from cortex.utils import tracing
samples = tracing.snapshot()
```

No-op when the env var is unset.

## Measured numbers (Linux x86_64, 500 Hz, IPC, 256 B)

Wire floor (raw sync zmq + Poller, separate processes):

| | p50 | p99 |
|---|---|---|
| Sync | 190–360 µs | 430–900 µs |

The interesting number is same-process contention: a sync subscriber sharing a process with a CPU-bound asyncio loop. With moderate load (4 ms burn / 5 ms period, ~80 % of one core):

| Runtime | p50 | p99 | max |
|---|---|---|---|
| 3.14 (GIL) | 530 µs | 1180 µs | **4910 µs** |
| 3.14t (free-threaded, `PYTHON_GIL=0`) | 730 µs | 1130 µs | **1520 µs** |

Median is slightly higher on the free-threaded build (single-thread CPython is slower without biased-locking) but **max is 3× tighter**. Control loops live or die by p99.9 / max, not median.

Reproduce with `benchmarks/bench_latency_inproc.py`. See [Benchmarks](benchmarks.md) for the full matrix.

## Recap

- `mode="async"` for telemetry, throughput, and integration with asyncio code.
- `mode="sync"` for control loops where jitter dominates correctness.
- `python3.14t` to clear sub-100 µs under in-process contention.
- Sync callbacks must be `def`, not `async def`. Caught at construction.
