# Cortex Design Review

This is a candid review of the current Cortex architecture as it exists today.

## What The Design Gets Right

### 1. The mental model is clean

`Node`, `Publisher`, `Subscriber`, and `Executor` are easy to understand. That matters in robotics because most teams are already debugging hardware, timing, and perception stacks. They do not need a messaging library that is conceptually expensive.

### 2. ZeroMQ over IPC is a good local-first choice

For single-machine robotics workloads, IPC sockets are a practical baseline:

- deployment is simple,
- dependencies are light,
- latency is decent,
- failure modes are understandable.

This is substantially more pragmatic than trying to start with a distributed RPC stack.

### 3. Message typing is lightweight but useful

The fingerprinting approach gives you a useful form of type identity without requiring code generation. For a Python-first system, that is a reasonable tradeoff.

### 4. The design is honest about being Pythonic

This library is not pretending to be a hard-real-time transport. That is good. A lot of robotics middleware gets worse when it makes timing promises the language runtime cannot actually keep.

## What The Design Gets Wrong

### 1. The discovery daemon is a single chokepoint

The discovery plane is centralized, synchronous, and stateful. That is acceptable for a toy system or a small workstation setup. It is not a strong foundation for a larger graph.

Problems:

- one daemon is a single point of failure,
- REQ/REP forces lockstep request handling,
- startup storms can serialize on a single socket,
- there is no notion of leases, heartbeats, or stale publisher reclamation beyond best-effort unregister.

This will become painful the moment processes crash uncleanly, restart frequently, or run across multiple machines.

### 2. Topic ownership is effectively one-publisher-per-topic

That is a serious limitation. Many robotics systems eventually need:

- redundant publishers,
- replicated sensors,
- failover,
- multiple producers feeding a shared stream.

The current registry rejects competing publishers by topic. That is simple, but it is also a structural dead end for anything beyond a tightly controlled single-producer graph.

### 3. The transport is local-only in practice

The architecture and defaults are built around IPC paths under `/tmp`. That is fine for a workstation. It does not scale to:

- multi-host robots,
- edge-to-base-station links,
- containerized deployments with namespace boundaries,
- cloud or cluster processing.

The code says “distributed systems” in spirit, but the design is really “single-host process graph with discovery.” Those are not the same thing.

### 4. Backpressure policy is underspecified

Queue size exists, but the system does not expose a strong contract about what happens under overload.

Important unanswered questions:

- Are old messages dropped or new ones?
- Should publishers block or fail fast?
- Should subscribers conflate or queue?
- What is the intended behavior for high-rate sensors versus low-rate control topics?

Without explicit backpressure semantics, the system behaves however ZeroMQ happens to behave under current socket settings. That is not design. That is delegation.

### 5. Callback execution is too naive for serious pipelines

Subscribers process callbacks in a simple async loop. That works for demos. It breaks down when callbacks become non-trivial.

Problems:

- one slow callback can stall message handling,
- there is no built-in bounded work queue,
- there is no separation between receive, decode, and user callback execution,
- there is no scheduler policy per topic.

As graphs grow, this turns into timing jitter and head-of-line blocking.

### 6. Message compatibility is brittle

Fingerprinting based on Python field structure is convenient, but it is not a robust schema evolution story.

You do not currently have a first-class answer for:

- optional fields added over time,
- backwards-compatible evolution,
- deprecation windows,
- cross-language interoperability,
- stable wire contracts.

This is acceptable for internal experiments. It is weak for long-lived systems.

## Scaling Problems You Will Hit

### 1. Process count scaling

As node count rises, discovery traffic and socket management get noisier. Even if steady-state data bypasses discovery, startup and restart behavior will degrade first.

### 2. Topic count scaling

Per-topic socket endpoints become operationally messy at larger scale. Many topics means many IPC files, many bindings, and more cleanup edge cases after crashes.

### 3. Payload size scaling

Large images, point clouds, and model tensors will keep stressing Python object creation, buffer ownership, and callback scheduling even after the current serialization improvements. Shared memory is not required yet, but eventually this design will force you toward it.

### 4. Multi-machine scaling

The current design will need more than minor edits to become a robust networked middleware. You will need to rethink:

- discovery,
- transport security,
- reconnection semantics,
- publisher liveness,
- addressability,
- observability.

### 5. Reliability scaling

There is little explicit modeling of delivery guarantees, replay, durability, or health monitoring. For robotics this is survivable in best-effort telemetry. It is much less acceptable for supervisory control, autonomy arbitration, or safety-critical status propagation.

## Brutal Summary

Cortex is a good small-system local IPC library.

It is not yet a mature robotics middleware.

Its strengths are simplicity, low ceremony, and a sensible local-first transport choice. Its weaknesses are exactly the ones that appear when a clean prototype meets scale: centralized discovery, simplistic scheduling, underspecified overload behavior, weak schema evolution, and single-host assumptions baked into the architecture.

If the goal is a fast Python-native pub/sub layer for one machine or a tightly managed robot process graph, the design is good enough and now materially faster on large payloads.

If the goal is something that competes with established robotics middleware as systems become larger, more distributed, and more failure-prone, the current design will run into hard walls rather than soft inefficiencies.

## Concrete Architectural Next Steps

1. Define transport semantics per topic class: telemetry, control, state, bulk sensor, debug.
2. Introduce explicit backpressure and drop policies instead of relying on implicit socket behavior.
3. Add publisher liveness or lease-based discovery so stale registrations disappear automatically.
4. Decouple receive, decode, and callback execution with bounded queues.
5. Decide whether Cortex is intentionally single-host or genuinely multi-host, then design for that decision instead of straddling both.
6. Add a real schema evolution strategy before message types spread across projects.