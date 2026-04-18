# Transport & QoS

*Stub — deep dive coming in a later pass.*

## Current socket settings

| Socket       | Option        | Value | Notes                                 |
| ------------ | ------------- | ----- | ------------------------------------- |
| Publisher PUB | `SNDHWM`     | 10 (default `queue_size`) | Drops under backpressure |
| Publisher PUB | `LINGER`     | 0     | Immediate close                       |
| Subscriber SUB | `RCVHWM`    | 10    | Oldest messages evicted when full     |
| Subscriber SUB | `LINGER`    | 0     |                                       |
| Daemon REP    | `RCVTIMEO`   | 1000 ms | Allows Ctrl-C responsiveness        |
| Daemon REP    | `LINGER`     | 0     |                                       |

## Today's delivery semantics

- Publisher uses `zmq.NOBLOCK`: if the send queue is full, the message is
  **silently dropped**.
- Subscriber HWM is a ring buffer: old messages are **silently evicted** on
  overflow.

This is fine for best-effort telemetry. It is unsafe for control commands.

## Planned QoS profiles

Taking inspiration from DDS, three profiles are enough for most robotics use:

- `best_effort_latest` — conflate; keep only newest (camera frames).
- `reliable_queue` — publisher blocks or errors (control commands).
- `dropping_queue` — current behavior with an exposed drop counter (telemetry).

See [critique.md § 4](../critique.md) for rationale.
