# Benchmarks

Cortex ships an in-repo benchmark suite at [`benchmarks/`](https://github.com/sudoRicheek/cortex/tree/main/benchmarks).

## Run

```bash
# Terminal 1
cortex-discovery

# Terminal 2
python benchmarks/bench_all.py --output results.json
```

Individual benchmarks:

- `benchmarks/bench_latency.py` — one-way publisher→subscriber latency.
- `benchmarks/bench_throughput.py` — messages/sec and MB/sec.
- `benchmarks/bench_all.py` — full matrix with summary and optional JSON dump.

## Reading results

- `p99` is what matters for real-time-ish workloads; `mean` can hide jitter.
- For array workloads, `MB/s` approaching memcpy bandwidth is a good sign
  that zero-copy transport is working.
- Serialization overhead via `inproc` sockets with `copy=False` is reported
  separately — that isolates the encode/decode path from the network path.

## Tips

- Pin publisher and subscriber to separate cores for stable latency numbers.
- Disable Turbo-Boost / set CPU governor to `performance` for reproducible
  runs.
- Always measure with the discovery daemon also running (it is off the hot
  path but can steal a little cache).
