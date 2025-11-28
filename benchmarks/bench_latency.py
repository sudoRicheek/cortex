#!/usr/bin/env python3
"""
Latency benchmark for Cortex.

Measures round-trip latency between publisher and subscriber.
"""

from __future__ import annotations

import argparse
import asyncio
import multiprocessing as mp
import statistics

# Add parent to path for imports
import sys
import time
from dataclasses import dataclass
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent.parent / "src"))

from cortex.core.publisher import Publisher
from cortex.core.subscriber import Subscriber
from cortex.discovery.daemon import DiscoveryDaemon
from cortex.messages.base import Message


@dataclass
class LatencyMessage(Message):
    """Message with timestamp for latency measurement."""

    send_time_ns: int
    sequence: int
    payload: bytes  # Variable size payload


def run_discovery_daemon():
    """Run the discovery daemon in a separate process."""
    daemon = DiscoveryDaemon()
    daemon.start()


def run_publisher(
    topic: str,
    num_messages: int,
    payload_size: int,
    rate_hz: float,
    ready_event,
    start_event,
):
    """Publisher process."""
    time.sleep(0.5)  # Wait for discovery daemon

    pub = Publisher(
        topic_name=topic,
        message_type=LatencyMessage,
        node_name="latency_publisher",
    )

    # Signal ready
    ready_event.set()

    # Wait for start signal
    start_event.wait()

    payload = b"\x00" * payload_size
    interval = 1.0 / rate_hz if rate_hz > 0 else 0

    for i in range(num_messages):
        msg = LatencyMessage(
            send_time_ns=time.time_ns(),
            sequence=i,
            payload=payload,
        )
        pub.publish(msg)

        if interval > 0:
            time.sleep(interval)

    # Send final message to signal completion
    time.sleep(0.1)
    pub.close()


def run_subscriber(
    topic: str,
    num_messages: int,
    ready_event,
    start_event,
    results_queue,
):
    """Subscriber process - runs async receive loop."""

    async def subscriber_main():
        await asyncio.sleep(0.5)  # Wait for discovery daemon

        latencies: list[float] = []
        received = 0

        sub = Subscriber(
            topic_name=topic,
            message_type=LatencyMessage,
            node_name="latency_subscriber",
            wait_for_topic=True,
            topic_timeout=10.0,
        )

        # Signal ready
        ready_event.set()

        # Wait for start signal
        start_event.wait()

        start_time = time.time()
        timeout = 30.0  # Max wait time

        while received < num_messages and (time.time() - start_time) < timeout:
            try:
                result = await asyncio.wait_for(sub.receive(), timeout=1.0)

                if result:
                    msg, _header = result
                    receive_time_ns = time.time_ns()
                    latency_us = (receive_time_ns - msg.send_time_ns) / 1000.0
                    latencies.append(latency_us)
                    received += 1
            except asyncio.TimeoutError:
                continue

        sub.close()

        # Send results back
        results_queue.put(
            {
                "received": received,
                "latencies": latencies,
            }
        )

    asyncio.run(subscriber_main())


def run_latency_benchmark(
    num_messages: int = 1000,
    payload_size: int = 1024,
    rate_hz: float = 1000.0,
) -> dict:
    """
    Run the latency benchmark.

    Args:
        num_messages: Number of messages to send
        payload_size: Size of payload in bytes
        rate_hz: Publishing rate (0 for unlimited)

    Returns:
        Dictionary with benchmark results
    """
    topic = "/benchmark/latency"

    # Start discovery daemon
    discovery_proc = mp.Process(target=run_discovery_daemon, daemon=True)
    discovery_proc.start()
    time.sleep(1.0)  # Give daemon more time to start and bind socket

    # Events for synchronization
    pub_ready = mp.Event()
    sub_ready = mp.Event()
    start_event = mp.Event()
    results_queue = mp.Queue()

    # Start subscriber first
    sub_proc = mp.Process(
        target=run_subscriber,
        args=(topic, num_messages, sub_ready, start_event, results_queue),
    )
    sub_proc.start()

    # Start publisher
    pub_proc = mp.Process(
        target=run_publisher,
        args=(topic, num_messages, payload_size, rate_hz, pub_ready, start_event),
    )
    pub_proc.start()

    # Wait for both to be ready
    pub_ready.wait(timeout=10)
    sub_ready.wait(timeout=10)

    # Small delay for connection establishment
    time.sleep(0.2)

    # Start benchmark
    benchmark_start = time.time()
    start_event.set()

    # Wait for completion
    pub_proc.join(timeout=60)
    sub_proc.join(timeout=60)

    benchmark_duration = time.time() - benchmark_start

    # Get results
    results = results_queue.get(timeout=5)

    # Cleanup
    discovery_proc.terminate()
    discovery_proc.join(timeout=2)

    # Calculate statistics
    latencies = results["latencies"]

    if latencies:
        stats = {
            "num_messages": num_messages,
            "payload_size": payload_size,
            "rate_hz": rate_hz,
            "received": results["received"],
            "loss_rate": (num_messages - results["received"]) / num_messages * 100,
            "duration_s": benchmark_duration,
            "latency_min_us": min(latencies),
            "latency_max_us": max(latencies),
            "latency_mean_us": statistics.mean(latencies),
            "latency_median_us": statistics.median(latencies),
            "latency_std_us": statistics.stdev(latencies) if len(latencies) > 1 else 0,
            "latency_p50_us": np.percentile(latencies, 50),
            "latency_p90_us": np.percentile(latencies, 90),
            "latency_p99_us": np.percentile(latencies, 99),
            "throughput_msg_per_s": results["received"] / benchmark_duration,
        }
    else:
        stats = {
            "error": "No messages received",
            "received": 0,
        }

    return stats


def print_results(results: dict) -> None:
    """Print benchmark results in a formatted way."""
    print("\n" + "=" * 60)
    print("LATENCY BENCHMARK RESULTS")
    print("=" * 60)

    if "error" in results:
        print(f"ERROR: {results['error']}")
        return

    print("\nConfiguration:")
    print(f"  Messages:      {results['num_messages']:,}")
    print(f"  Payload size:  {results['payload_size']:,} bytes")
    print(f"  Target rate:   {results['rate_hz']:,.0f} Hz")

    print("\nDelivery:")
    print(f"  Received:      {results['received']:,} / {results['num_messages']:,}")
    print(f"  Loss rate:     {results['loss_rate']:.2f}%")
    print(f"  Duration:      {results['duration_s']:.2f} s")
    print(f"  Throughput:    {results['throughput_msg_per_s']:,.0f} msg/s")

    print("\nLatency (microseconds):")
    print(f"  Min:           {results['latency_min_us']:,.1f} µs")
    print(f"  Max:           {results['latency_max_us']:,.1f} µs")
    print(f"  Mean:          {results['latency_mean_us']:,.1f} µs")
    print(f"  Median:        {results['latency_median_us']:,.1f} µs")
    print(f"  Std Dev:       {results['latency_std_us']:,.1f} µs")
    print(f"  P50:           {results['latency_p50_us']:,.1f} µs")
    print(f"  P90:           {results['latency_p90_us']:,.1f} µs")
    print(f"  P99:           {results['latency_p99_us']:,.1f} µs")

    print("=" * 60 + "\n")


def main():
    parser = argparse.ArgumentParser(description="Cortex Latency Benchmark")
    parser.add_argument(
        "-n",
        "--num-messages",
        type=int,
        default=1000,
        help="Number of messages to send (default: 1000)",
    )
    parser.add_argument(
        "-s",
        "--payload-size",
        type=int,
        default=1024,
        help="Payload size in bytes (default: 1024)",
    )
    parser.add_argument(
        "-r",
        "--rate",
        type=float,
        default=1000.0,
        help="Publishing rate in Hz, 0 for unlimited (default: 1000)",
    )

    args = parser.parse_args()

    print("\nRunning latency benchmark...")
    print(f"  Messages: {args.num_messages}")
    print(f"  Payload:  {args.payload_size} bytes")
    print(f"  Rate:     {args.rate} Hz")

    results = run_latency_benchmark(
        num_messages=args.num_messages,
        payload_size=args.payload_size,
        rate_hz=args.rate,
    )

    print_results(results)


if __name__ == "__main__":
    main()
