#!/usr/bin/env python3
"""
Same-process latency benchmark.

The other latency benchmarks split publisher and subscriber into separate
OS processes, which means each side has its own GIL and the
free-threaded build of CPython buys nothing on those numbers.

In a real robotics node you typically have an asyncio loop running
timers, telemetry subscribers, and CPU-bound housekeeping (state
estimation, logging, planning) **alongside** the low-latency control
subscriber. They share a process. On stock CPython the asyncio thread
holds the GIL between sleeps; the sync receive thread spends time
*waiting* for that GIL even when the kernel has woken it. On
``python3.14t`` (PEP 779) there is no GIL and the receive thread is
free to run on a separate core without contention.

This benchmark stresses exactly that scenario:

* main thread runs an asyncio loop with a periodic CPU-bound job
* a publisher publishes to an IPC topic at a fixed rate from another
  thread
* a ``ThreadedSubscriber`` receives them on a third thread

The reported latency is the wall-clock difference between
``send_time_ns`` (set by the publisher) and the moment the sync
callback runs.
"""

import argparse
import asyncio
import contextlib
import statistics
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent.parent / "src"))

from cortex.core.publisher import Publisher  # noqa: E402
from cortex.core.sync_subscriber import ThreadedSubscriber  # noqa: E402
from cortex.discovery.daemon import DiscoveryDaemon  # noqa: E402
from cortex.messages.base import Message  # noqa: E402
from cortex.utils.runtime import runtime_info  # noqa: E402


@dataclass
class TickMessage(Message):
    """Carries publisher-side perf_counter_ns timestamp + optional payload.

    The ``payload`` is deliberately ``bytes`` (not numpy) so the wire
    cost is dominated by the value the caller picks via ``--payload-size``.
    Header + two ``int`` fields contribute ~40 bytes of fixed overhead.
    """

    send_time_ns: int
    sequence: int
    payload: bytes


def _cpu_burn_ms(target_ms: float) -> None:
    """Burn ~``target_ms`` of CPU time. Used to keep the asyncio thread busy."""
    end = time.perf_counter() + target_ms / 1000.0
    x = 0
    while time.perf_counter() < end:
        # Trivial work the JIT can't elide; exercises the GIL on stock CPython.
        x = (x * 1664525 + 1013904223) & 0xFFFFFFFF


async def _async_busy_loop(period_s: float, burn_ms: float, stop: asyncio.Event):
    """Periodic CPU-bound asyncio task — emulates planner / state estimator."""
    while not stop.is_set():
        _cpu_burn_ms(burn_ms)
        with contextlib.suppress(asyncio.TimeoutError):
            await asyncio.wait_for(stop.wait(), timeout=period_s)


def _publisher_thread(
    pub: Publisher,
    num_messages: int,
    rate_hz: float,
    payload: bytes,
    ready: threading.Event,
) -> None:
    """Sync publisher in a dedicated thread so it never fights the loop."""
    interval = 1.0 / rate_hz
    next_t = time.perf_counter()
    ready.set()
    for i in range(num_messages):
        next_t += interval
        sleep = next_t - time.perf_counter()
        if sleep > 0:
            time.sleep(sleep)
        pub.publish(
            TickMessage(
                send_time_ns=time.perf_counter_ns(),
                sequence=i,
                payload=payload,
            )
        )


async def run_benchmark(
    num_messages: int,
    rate_hz: float,
    burn_ms: float,
    busy_period_s: float,
    payload_size: int,
) -> dict:
    """Run pub + sync sub + busy asyncio task all inside one process."""
    daemon_thread = threading.Thread(
        target=DiscoveryDaemon().start, name="bench-discovery", daemon=True
    )
    daemon_thread.start()
    await asyncio.sleep(0.5)

    topic = "/bench/inproc"
    pub = Publisher(
        topic_name=topic,
        message_type=TickMessage,
        node_name="inproc_pub",
    )
    await asyncio.sleep(0.2)

    payload = b"\x00" * payload_size
    latencies_ns: list[int] = []
    seen_lock = threading.Lock()

    def cb(msg: TickMessage, _hdr) -> None:
        recv = time.perf_counter_ns()
        with seen_lock:
            latencies_ns.append(recv - msg.send_time_ns)

    sub = ThreadedSubscriber(
        topic_name=topic,
        message_type=TickMessage,
        callback=cb,
        queue_size=64,
        topic_timeout=5.0,
    )
    sub.start()

    # Warmup until SUB filter has propagated.
    deadline = time.monotonic() + 3.0
    seq = -1
    while time.monotonic() < deadline:
        with seen_lock:
            if latencies_ns:
                break
        pub.publish(
            TickMessage(
                send_time_ns=time.perf_counter_ns(),
                sequence=seq,
                payload=payload,
            )
        )
        seq -= 1
        await asyncio.sleep(0.01)
    with seen_lock:
        latencies_ns.clear()

    # Start the asyncio CPU-burn loop alongside the receiver.
    stop_event = asyncio.Event()
    busy_task = asyncio.create_task(
        _async_busy_loop(busy_period_s, burn_ms, stop_event)
    )

    pub_ready = threading.Event()
    pub_thread = threading.Thread(
        target=_publisher_thread,
        args=(pub, num_messages, rate_hz, payload, pub_ready),
        name="bench-pub",
    )
    pub_thread.start()
    pub_ready.wait(timeout=2.0)

    bench_start = time.perf_counter()
    pub_thread.join(timeout=120)
    # Allow last-message delivery to settle.
    await asyncio.sleep(0.2)
    bench_end = time.perf_counter()

    stop_event.set()
    await busy_task

    sub.close()
    pub.close()

    if not latencies_ns:
        return {"error": "no messages received"}

    duration = bench_end - bench_start
    arr_us = [ns / 1000.0 for ns in latencies_ns]
    received = len(arr_us)
    return {
        "received": received,
        "duration_s": duration,
        "payload_size": payload_size,
        "throughput_msg_per_s": received / duration if duration else 0,
        "throughput_bytes_per_s": (received * payload_size / duration)
        if duration
        else 0,
        "latency_min_us": min(arr_us),
        "latency_p50_us": float(np.percentile(arr_us, 50)),
        "latency_mean_us": statistics.mean(arr_us),
        "latency_p90_us": float(np.percentile(arr_us, 90)),
        "latency_p99_us": float(np.percentile(arr_us, 99)),
        "latency_p999_us": float(np.percentile(arr_us, 99.9)),
        "latency_max_us": max(arr_us),
        "dropped": sub.dropped_count,
    }


def _human_byte_rate(bytes_per_s: float) -> str:
    """Format bytes/sec as KB/s, MB/s, or GB/s."""
    if bytes_per_s >= 1024**3:
        return f"{bytes_per_s / 1024**3:,.2f} GB/s"
    if bytes_per_s >= 1024**2:
        return f"{bytes_per_s / 1024**2:,.2f} MB/s"
    if bytes_per_s >= 1024:
        return f"{bytes_per_s / 1024:,.2f} KB/s"
    return f"{bytes_per_s:,.0f} B/s"


def print_results(results: dict, num_messages: int, rate_hz: float, burn_ms: float):
    info = runtime_info()
    label = "free-threaded" if info.free_threaded else "GIL"
    print("\n" + "=" * 60)
    print(
        f"INPROC LATENCY (CPython {info.python_version[0]}.{info.python_version[1]} {label})"
    )
    print("=" * 60)
    print(
        f"Pub rate: {rate_hz} Hz, msgs: {num_messages}, asyncio burn: {burn_ms} ms/period"
    )
    if "error" in results:
        print(f"ERROR: {results['error']}")
        return
    print(
        f"Received: {results['received']:,} / {num_messages:,}  Drops: {results['dropped']}"
    )
    print(f"Duration: {results['duration_s']:.2f}s")
    print(
        f"Throughput: {results['throughput_msg_per_s']:,.0f} msg/s  "
        f"({_human_byte_rate(results['throughput_bytes_per_s'])} payload, "
        f"{results['payload_size']:,} B/msg)"
    )
    print("\nLatency (µs):")
    print(f"  min:    {results['latency_min_us']:>9.1f}")
    print(f"  p50:    {results['latency_p50_us']:>9.1f}")
    print(f"  mean:   {results['latency_mean_us']:>9.1f}")
    print(f"  p90:    {results['latency_p90_us']:>9.1f}")
    print(f"  p99:    {results['latency_p99_us']:>9.1f}")
    print(f"  p99.9:  {results['latency_p999_us']:>9.1f}")
    print(f"  max:    {results['latency_max_us']:>9.1f}")
    print("=" * 60 + "\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("-n", "--num-messages", type=int, default=2000)
    parser.add_argument("-r", "--rate", type=float, default=500.0)
    parser.add_argument(
        "-s",
        "--payload-size",
        type=int,
        default=256,
        help="Bytes of payload per message (default: 256)",
    )
    parser.add_argument(
        "--burn-ms",
        type=float,
        default=2.0,
        help="CPU-burn time per asyncio busy iteration (ms)",
    )
    parser.add_argument(
        "--busy-period",
        type=float,
        default=0.005,
        help="Period between asyncio busy iterations (s)",
    )
    args = parser.parse_args()

    results = asyncio.run(
        run_benchmark(
            num_messages=args.num_messages,
            rate_hz=args.rate,
            burn_ms=args.burn_ms,
            busy_period_s=args.busy_period,
            payload_size=args.payload_size,
        )
    )
    print_results(results, args.num_messages, args.rate, args.burn_ms)


if __name__ == "__main__":
    main()
