#!/usr/bin/env python3
"""
Synchronous-path latency benchmark for Cortex.

Establishes the latency floor achievable on this hardware when the
subscriber side runs on a dedicated OS thread with synchronous zmq +
``zmq.Poller``, bypassing the asyncio scheduler entirely.

Companion to ``bench_latency.py``. The publisher is identical (sync zmq),
so any difference in p50/p99 between the two scripts is purely the cost
of ``zmq.asyncio`` + ``asyncio`` + ``await user_callback`` on the
subscriber side.

Use:
    python benchmarks/bench_latency_sync.py -n 5000 -s 1024 -r 1000

On a free-threaded build (``python3.14t``) the same script will report
markedly tighter p99 because the receive thread does not contend with
any asyncio loop for the GIL.
"""

import argparse
import multiprocessing as mp
import statistics
import sys
import time
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import zmq

sys.path.insert(0, str(Path(__file__).parent.parent / "src"))

from cortex.core.publisher import Publisher  # noqa: E402
from cortex.discovery.daemon import DiscoveryDaemon  # noqa: E402
from cortex.messages.base import Message  # noqa: E402
from cortex.utils.runtime import runtime_info  # noqa: E402


@dataclass
class LatencyMessage(Message):
    """Message with a wall-clock send timestamp for one-way latency."""

    send_time_ns: int
    sequence: int
    payload: bytes


def _run_discovery_daemon() -> None:
    DiscoveryDaemon().start()


def _run_publisher(
    topic: str,
    num_messages: int,
    payload_size: int,
    rate_hz: float,
    ready_event,
    start_event,
) -> None:
    time.sleep(0.5)

    pub = Publisher(
        topic_name=topic,
        message_type=LatencyMessage,
        node_name="latency_publisher_sync",
    )
    ready_event.set()
    start_event.wait()

    payload = b"\x00" * payload_size
    interval = 1.0 / rate_hz if rate_hz > 0 else 0.0

    for i in range(num_messages):
        msg = LatencyMessage(
            send_time_ns=time.time_ns(),
            sequence=i,
            payload=payload,
        )
        pub.publish(msg)
        if interval > 0.0:
            time.sleep(interval)

    time.sleep(0.1)
    pub.close()


def _run_sync_subscriber(
    topic: str,
    num_messages: int,
    ready_event,
    start_event,
    results_queue,
) -> None:
    """Pure-sync receive loop: zmq.Context + zmq.Poller, no asyncio."""
    from cortex.discovery.client import DiscoveryClient  # local import

    discovery = DiscoveryClient()
    info = discovery.wait_for_topic(topic, timeout=30.0)
    if info is None:
        results_queue.put({"received": 0, "latencies": [], "error": "topic_timeout"})
        discovery.close()
        return
    discovery.close()

    ctx = zmq.Context.instance()
    sock = ctx.socket(zmq.SUB)
    sock.setsockopt(zmq.RCVHWM, 10)
    sock.setsockopt(zmq.LINGER, 0)
    sock.setsockopt_string(zmq.SUBSCRIBE, topic)
    sock.connect(info.address)

    poller = zmq.Poller()
    poller.register(sock, zmq.POLLIN)

    ready_event.set()
    start_event.wait()

    latencies: list[float] = []
    deadline = time.monotonic() + 60.0

    while len(latencies) < num_messages and time.monotonic() < deadline:
        events = dict(poller.poll(timeout=1000))
        if sock not in events:
            continue
        frames = sock.recv_multipart(copy=False, flags=zmq.NOBLOCK)
        if len(frames) < 2:
            continue
        payload_frames = frames[1:]
        if len(payload_frames) == 1:
            raw = memoryview(payload_frames[0].buffer)
            msg, _hdr = LatencyMessage.from_bytes(raw)
        else:
            msg, _hdr = LatencyMessage.from_frames(payload_frames)
        recv_ns = time.time_ns()
        latencies.append((recv_ns - msg.send_time_ns) / 1000.0)

    sock.close()
    ctx.term()
    results_queue.put({"received": len(latencies), "latencies": latencies})


def run_sync_benchmark(
    num_messages: int = 1000,
    payload_size: int = 1024,
    rate_hz: float = 1000.0,
) -> dict:
    topic = "/benchmark/latency_sync"

    discovery_proc = mp.Process(target=_run_discovery_daemon, daemon=True)
    discovery_proc.start()
    time.sleep(1.0)

    pub_ready = mp.Event()
    sub_ready = mp.Event()
    start_event = mp.Event()
    results_queue: mp.Queue = mp.Queue()

    sub_proc = mp.Process(
        target=_run_sync_subscriber,
        args=(topic, num_messages, sub_ready, start_event, results_queue),
    )
    sub_proc.start()

    pub_proc = mp.Process(
        target=_run_publisher,
        args=(topic, num_messages, payload_size, rate_hz, pub_ready, start_event),
    )
    pub_proc.start()

    pub_ready.wait(timeout=10)
    sub_ready.wait(timeout=15)
    time.sleep(0.2)

    bench_start = time.time()
    start_event.set()
    pub_proc.join(timeout=120)
    sub_proc.join(timeout=120)
    duration = time.time() - bench_start

    results = results_queue.get(timeout=5)
    discovery_proc.terminate()
    discovery_proc.join(timeout=2)

    latencies = results["latencies"]
    if not latencies:
        return {"error": results.get("error", "no messages"), "received": 0}

    return {
        "num_messages": num_messages,
        "payload_size": payload_size,
        "rate_hz": rate_hz,
        "received": results["received"],
        "loss_rate": (num_messages - results["received"]) / num_messages * 100,
        "duration_s": duration,
        "latency_min_us": min(latencies),
        "latency_max_us": max(latencies),
        "latency_mean_us": statistics.mean(latencies),
        "latency_median_us": statistics.median(latencies),
        "latency_std_us": statistics.stdev(latencies) if len(latencies) > 1 else 0.0,
        "latency_p50_us": float(np.percentile(latencies, 50)),
        "latency_p90_us": float(np.percentile(latencies, 90)),
        "latency_p99_us": float(np.percentile(latencies, 99)),
        "throughput_msg_per_s": results["received"] / duration if duration else 0,
        # Byte throughput approximates only the user-supplied payload, not
        # the 24-byte header or msgpack metadata. Good enough to compare
        # benches at different ``payload_size`` settings.
        "throughput_bytes_per_s": (
            results["received"] * payload_size / duration if duration else 0
        ),
    }


def _human_byte_rate(bytes_per_s: float) -> str:
    """Format a bytes/sec rate as KB/s, MB/s, or GB/s."""
    if bytes_per_s >= 1024**3:
        return f"{bytes_per_s / 1024**3:,.2f} GB/s"
    if bytes_per_s >= 1024**2:
        return f"{bytes_per_s / 1024**2:,.2f} MB/s"
    if bytes_per_s >= 1024:
        return f"{bytes_per_s / 1024:,.2f} KB/s"
    return f"{bytes_per_s:,.0f} B/s"


def print_results(results: dict) -> None:
    info = runtime_info()
    print("\n" + "=" * 60)
    print("SYNC LATENCY BENCHMARK (raw zmq.Poller, no asyncio)")
    print("=" * 60)
    print(
        f"Runtime: {info.implementation} "
        f"{info.python_version[0]}.{info.python_version[1]}.{info.python_version[2]} "
        f"(GIL {'disabled' if info.free_threaded else 'enabled'})"
    )

    if "error" in results:
        print(f"ERROR: {results['error']}")
        return

    print("\nDelivery:")
    print(f"  Received:      {results['received']:,} / {results['num_messages']:,}")
    print(f"  Loss rate:     {results['loss_rate']:.2f}%")
    print(f"  Payload size:  {results['payload_size']:,} bytes/msg")
    print(f"  Throughput:    {results['throughput_msg_per_s']:,.0f} msg/s")
    print(f"  Bytes/s:       {_human_byte_rate(results['throughput_bytes_per_s'])}")

    print("\nLatency (microseconds):")
    print(f"  Min:           {results['latency_min_us']:,.1f}")
    print(f"  Median (p50):  {results['latency_p50_us']:,.1f}")
    print(f"  Mean:          {results['latency_mean_us']:,.1f}")
    print(f"  P90:           {results['latency_p90_us']:,.1f}")
    print(f"  P99:           {results['latency_p99_us']:,.1f}")
    print(f"  Max:           {results['latency_max_us']:,.1f}")
    print("=" * 60 + "\n")


def main() -> None:
    parser = argparse.ArgumentParser(description="Cortex sync latency benchmark")
    parser.add_argument("-n", "--num-messages", type=int, default=1000)
    parser.add_argument("-s", "--payload-size", type=int, default=1024)
    parser.add_argument("-r", "--rate", type=float, default=1000.0)
    args = parser.parse_args()

    print_results(
        run_sync_benchmark(
            num_messages=args.num_messages,
            payload_size=args.payload_size,
            rate_hz=args.rate,
        )
    )


if __name__ == "__main__":
    main()
