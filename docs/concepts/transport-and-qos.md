# Transport & QoS

## Socket settings

| Socket       | Option        | Value | Notes                                 |
| ------------ | ------------- | ----- | ------------------------------------- |
| Publisher PUB | `SNDHWM`     | 10 (default `queue_size`) | Drops under backpressure |
| Publisher PUB | `LINGER`     | 0     | Immediate close                       |
| Subscriber SUB | `RCVHWM`    | 10    | Oldest messages evicted when full     |
| Subscriber SUB | `LINGER`    | 0     |                                       |
| Daemon REP    | `RCVTIMEO`   | 1000 ms | Keeps Ctrl-C responsive             |
| Daemon REP    | `LINGER`     | 0     |                                       |

## Delivery semantics

- Publisher uses `zmq.NOBLOCK`: if the send queue is full, the message is **silently dropped**.
- Subscriber HWM is a ring buffer: oldest messages are **silently evicted** on overflow.

Fine for best-effort telemetry. Not safe for control commands without an application-level ack — the bytes can be lost on either side.
