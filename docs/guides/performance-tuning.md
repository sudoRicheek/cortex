# Performance tuning

Current measured numbers on the repo's benchmark suite (single workstation):

| Workload              | Throughput / latency            |
| --------------------- | ------------------------------- |
| Small payload latency | mean 556 µs, p99 1075 µs        |
| 1MB array throughput  | 7.7k msg/s, 8.0 GB/s            |
| 4MB array throughput  | 2.25k msg/s, 9.4 GB/s           |
| 1080p RGB             | 1422 fps, 8.8 GB/s              |

See [Benchmarks guide](benchmarks.md) to reproduce.

## Copy-on-use

Decoded NumPy arrays **alias the ZMQ frame memory**. That is what makes
large-payload throughput close to memcpy bandwidth — but it means:

- If you intend to mutate the array, `arr = arr.copy()` first.
- If you intend to hold the array past the callback, copy it first.

## Queue sizing

Per-socket HWM defaults to 10. Increase `queue_size` on high-rate producers
whose subscribers are known to be slow — but remember that ZMQ drops silently
at the HWM.

## When to prefer the inline path

Single tiny messages (primitives only, < 1 KB) see no benefit from multipart.
The inline `to_bytes` path is still fine there. Publishers always use
multipart today.

## uvloop

Installed by default on Unix. Drops tail latency on high-rate small messages
noticeably. No action needed.
