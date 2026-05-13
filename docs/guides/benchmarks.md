# Benchmarks

Suite at [`benchmarks/`](https://github.com/sudoRicheek/cortex/tree/main/benchmarks).

## Run

```bash
# terminal 1
cortex-discovery

# terminal 2
python benchmarks/bench_all.py --output results.json
```

Individual benchmarks:

- `bench_latency.py` — one-way publisher→subscriber latency (async).
- `bench_latency_sync.py` — raw sync zmq baseline (no asyncio).
- `bench_latency_inproc.py` — in-process pub + sync sub + a CPU-bound asyncio neighbour, to expose GIL contention.
- `bench_throughput.py` — messages/sec and MB/sec.
- `bench_all.py` — full matrix; JSON output via `--output`.

## Reading results

- **p99** is what matters for real-time workloads; the mean hides jitter.
- For array workloads, `MB/s` approaching memcpy bandwidth means zero-copy transport is working.
- Serialization overhead via `inproc` sockets with `copy=False` is reported separately to isolate encode/decode from the network path.
