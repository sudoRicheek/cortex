# Performance tuning

Measured on the repo's benchmark suite, single workstation:

| Workload              | Throughput / latency            |
| --------------------- | ------------------------------- |
| Small payload latency | mean 556 µs, p99 1075 µs        |
| 1 MB array throughput | 7.7k msg/s, 8.0 GB/s            |
| 4 MB array throughput | 2.25k msg/s, 9.4 GB/s           |
| 1080p RGB             | 1422 fps, 8.8 GB/s              |

See [Benchmarks](benchmarks.md) to reproduce.

## Copy-on-use

Decoded NumPy arrays **alias the ZMQ frame memory** — that's how large-payload throughput hits memcpy bandwidth. Consequence:

- Mutate the array? `arr = arr.copy()` first.
- Hold the array past the callback? Copy first.

## Queue sizing

Per-socket HWM defaults to 10. Increase `queue_size` on high-rate producers whose subscribers are slow — but remember ZMQ drops silently at the HWM.

## When to prefer the single-blob path

Tiny messages (primitives only, < 1 KB) see no benefit from multipart. The inline `to_bytes` path is fine there. The transport always uses multipart today.

## uvloop

Installed by default on Unix. Drops tail latency on high-rate small messages.

## Sync subscriber mode

For control loops needing sub-100 µs p99, see [Sync subscriber mode](sync-mode.md). Bypasses asyncio + `zmq.asyncio` on the receive path.
