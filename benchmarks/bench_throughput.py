#!/usr/bin/env python3
"""
Throughput benchmark for Cortex framework.

Measures maximum message throughput for different payload sizes.
"""

import asyncio
import builtins
import contextlib
import logging
import threading
import time
from dataclasses import dataclass
from typing import Any

import numpy as np

from cortex import Publisher, Subscriber
from cortex.discovery import DiscoveryDaemon
from cortex.messages import ArrayMessage
from cortex.messages.base import MessageHeader

# Reduce logging noise during benchmarks
logging.getLogger("cortex").setLevel(logging.WARNING)


@dataclass
class ThroughputResult:
    """Results from a throughput test."""

    payload_size: int
    messages_sent: int
    messages_received: int
    duration: float
    throughput_msgs: float  # messages per second
    throughput_bytes: float  # bytes per second
    throughput_mbps: float  # megabits per second
    loss_rate: float


def run_throughput_test(
    payload_size: int,
    duration_seconds: float = 5.0,
    discovery_address: str = "ipc:///tmp/cortex_benchmark_discovery",
) -> ThroughputResult:
    """
    Run a throughput test with given payload size.

    Args:
        payload_size: Size of payload in bytes
        duration_seconds: How long to run the test
        discovery_address: Discovery daemon address

    Returns:
        ThroughputResult with benchmark data
    """
    topic = "/benchmark/throughput"

    # Create payload (numpy array of given size)
    # Each float64 is 8 bytes
    num_elements = max(1, payload_size // 8)
    payload = np.random.rand(num_elements).astype(np.float64)
    actual_payload_size = payload.nbytes

    # Counters
    received_count = 0
    lock = threading.Lock()

    # Subscriber callback (async)
    async def on_message(msg: ArrayMessage, header: MessageHeader) -> None:
        nonlocal received_count
        with lock:
            received_count += 1

    # Start discovery daemon
    daemon = DiscoveryDaemon(address=discovery_address)
    daemon_thread = threading.Thread(target=daemon.start, daemon=True)
    daemon_thread.start()
    time.sleep(1.0)  # Give daemon more time to start and bind socket

    sent_count = 0

    try:
        # Create publisher
        pub = Publisher(
            topic_name=topic,
            message_type=ArrayMessage,
            node_name="throughput_pub",
            discovery_address=discovery_address,
            queue_size=10000,  # Large queue for throughput test
        )

        # Create subscriber
        sub = Subscriber(
            topic_name=topic,
            message_type=ArrayMessage,
            callback=on_message,
            node_name="throughput_sub",
            discovery_address=discovery_address,
            queue_size=10000,
        )

        # Wait for connection
        time.sleep(0.5)

        # Start subscriber in background using asyncio
        sub_running = True

        def subscriber_loop():
            async def run_sub():
                sub.start()
                while sub_running:
                    try:
                        result = await asyncio.wait_for(sub.receive(), timeout=0.01)
                        if result and sub._callback:
                            msg, header = result
                            await sub._callback(msg, header)
                    except asyncio.TimeoutError:
                        pass
                    except Exception:
                        break

            asyncio.run(run_sub())

        sub_thread = threading.Thread(target=subscriber_loop, daemon=True)
        sub_thread.start()

        # Run publisher at maximum speed for specified duration
        start_time = time.perf_counter()
        end_time = start_time + duration_seconds

        message = ArrayMessage(data=payload)

        while time.perf_counter() < end_time:
            if pub.publish(message):
                sent_count += 1

        actual_duration = time.perf_counter() - start_time

        # Give subscriber time to catch up
        time.sleep(0.5)
        sub_running = False
        sub_thread.join(timeout=1.0)

        # Calculate results
        with lock:
            final_received = received_count

        throughput_msgs = final_received / actual_duration
        throughput_bytes = throughput_msgs * actual_payload_size
        throughput_mbps = (throughput_bytes * 8) / 1_000_000  # Convert to megabits

        loss_rate = 1.0 - (final_received / sent_count) if sent_count > 0 else 0.0

        return ThroughputResult(
            payload_size=actual_payload_size,
            messages_sent=sent_count,
            messages_received=final_received,
            duration=actual_duration,
            throughput_msgs=throughput_msgs,
            throughput_bytes=throughput_bytes,
            throughput_mbps=throughput_mbps,
            loss_rate=loss_rate,
        )

    finally:
        # Cleanup
        with contextlib.suppress(builtins.BaseException):
            pub.close()
        with contextlib.suppress(builtins.BaseException):
            sub.close()
        daemon.stop()


def run_throughput_benchmark(
    num_messages: int = 1000,
    array_shape: tuple[int, ...] = (100, 100),
    dtype: str = "float32",
) -> dict[str, Any]:
    """
    Run throughput benchmark (compatibility wrapper for bench_all.py).

    Args:
        num_messages: Number of messages to send
        array_shape: Shape of array to send
        dtype: NumPy dtype string

    Returns:
        Dictionary with benchmark results
    """
    import uuid

    # Calculate payload size
    test_array = np.zeros(array_shape, dtype=dtype)
    payload_size = test_array.nbytes

    # Estimate duration based on message count
    # Assume roughly 10000 msg/s for estimation
    duration = max(1.0, num_messages / 10000)

    # Use unique discovery address to avoid conflicts
    unique_id = uuid.uuid4().hex[:8]
    discovery_address = f"ipc:///tmp/cortex_bench_{unique_id}"

    result = run_throughput_test(
        payload_size=payload_size,
        duration_seconds=duration,
        discovery_address=discovery_address,
    )

    return {
        "num_messages": num_messages,
        "array_shape": array_shape,
        "dtype": dtype,
        "payload_size_bytes": result.payload_size,
        "messages_sent": result.messages_sent,
        "messages_received": result.messages_received,
        "duration_s": result.duration,
        "throughput_msg_per_s": result.throughput_msgs,
        "throughput_mb_per_s": result.throughput_bytes / 1_000_000,
        "loss_rate_percent": result.loss_rate * 100,
    }


def format_bytes(num_bytes: float) -> str:
    """Format bytes in human-readable form."""
    for unit in ["B", "KB", "MB", "GB"]:
        if abs(num_bytes) < 1024.0:
            return f"{num_bytes:.1f} {unit}"
        num_bytes /= 1024.0
    return f"{num_bytes:.1f} TB"


def format_rate(rate: float) -> str:
    """Format rate in human-readable form."""
    if rate >= 1_000_000:
        return f"{rate / 1_000_000:.2f}M"
    elif rate >= 1_000:
        return f"{rate / 1_000:.2f}K"
    else:
        return f"{rate:.0f}"


def main():
    """Run throughput benchmarks."""
    print("=" * 70)
    print("CORTEX THROUGHPUT BENCHMARK")
    print("=" * 70)
    print()

    # Test different payload sizes
    payload_sizes = [
        64,  # 64 B - small messages
        256,  # 256 B
        1024,  # 1 KB
        4096,  # 4 KB
        16384,  # 16 KB
        65536,  # 64 KB
        262144,  # 256 KB
        1048576,  # 1 MB - large messages
        4194304,  # 4 MB - very large (like images)
    ]

    duration = 3.0  # seconds per test
    results: list[ThroughputResult] = []

    print(f"Running throughput tests ({duration}s each)...")
    print()

    for i, size in enumerate(payload_sizes):
        print(
            f"  [{i + 1}/{len(payload_sizes)}] Testing {format_bytes(size)} payload...",
            end=" ",
            flush=True,
        )

        try:
            result = run_throughput_test(
                payload_size=size,
                duration_seconds=duration,
                discovery_address=f"ipc:///tmp/cortex_bench_{i}",
            )
            results.append(result)
            print(
                f"✓ {format_rate(result.throughput_msgs)}/s, {result.throughput_mbps:.1f} Mbps"
            )
        except Exception as e:
            print(f"✗ Error: {e}")

        time.sleep(0.5)  # Brief pause between tests

    # Print summary table
    print()
    print("=" * 70)
    print("RESULTS SUMMARY")
    print("=" * 70)
    print()
    print(
        f"{'Payload':<12} {'Sent':<10} {'Recv':<10} {'Loss':<8} {'Msg/s':<12} {'Throughput':<15}"
    )
    print("-" * 70)

    for r in results:
        print(
            f"{format_bytes(r.payload_size):<12} "
            f"{r.messages_sent:<10,} "
            f"{r.messages_received:<10,} "
            f"{r.loss_rate * 100:>5.1f}%  "
            f"{format_rate(r.throughput_msgs):<12}/s "
            f"{r.throughput_mbps:>8.1f} Mbps"
        )

    print("-" * 70)
    print()

    # Find peak throughput
    if results:
        peak_msgs = max(results, key=lambda r: r.throughput_msgs)
        peak_bytes = max(results, key=lambda r: r.throughput_bytes)

        print("Peak Performance:")
        print(
            f"  Messages:   {format_rate(peak_msgs.throughput_msgs)}/s @ {format_bytes(peak_msgs.payload_size)} payload"
        )
        print(
            f"  Bandwidth:  {peak_bytes.throughput_mbps:.1f} Mbps @ {format_bytes(peak_bytes.payload_size)} payload"
        )
        print(f"              ({peak_bytes.throughput_bytes / 1_000_000:.1f} MB/s)")

    print()
    print("=" * 70)


if __name__ == "__main__":
    main()
