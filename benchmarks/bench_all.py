#!/usr/bin/env python3
"""
Comprehensive benchmark suite for Cortex.

Runs all benchmarks and generates a summary report.
"""

import argparse
import json

# Add parent to path for imports
import sys
import time
from datetime import datetime
from pathlib import Path

import numpy as np
import zmq

sys.path.insert(0, str(Path(__file__).parent.parent / "src"))

from bench_latency import run_latency_benchmark
from bench_throughput import run_throughput_benchmark

from cortex.messages.standard import ArrayMessage


def run_all_benchmarks() -> dict:
    """Run the complete benchmark suite."""

    results = {
        "timestamp": datetime.now().isoformat(),
        "system_info": get_system_info(),
        "benchmarks": {},
    }

    print("\n" + "=" * 80)
    print("CORTEX BENCHMARK SUITE")
    print("=" * 80)

    # 1. Latency benchmarks
    print("\n[1/4] Running latency benchmarks...")

    latency_configs = [
        {
            "num_messages": 1000,
            "payload_size": 64,
            "rate_hz": 1000,
            "name": "small_payload",
        },
        {
            "num_messages": 1000,
            "payload_size": 1024,
            "rate_hz": 1000,
            "name": "medium_payload",
        },
        {
            "num_messages": 500,
            "payload_size": 65536,
            "rate_hz": 500,
            "name": "large_payload",
        },
        {"num_messages": 5000, "payload_size": 256, "rate_hz": 0, "name": "max_rate"},
    ]

    results["benchmarks"]["latency"] = {}
    for config in latency_configs:
        name = config.pop("name")
        print(f"  - {name}...", end=" ", flush=True)
        try:
            result = run_latency_benchmark(**config)
            results["benchmarks"]["latency"][name] = result
            if "error" not in result:
                print(
                    f"mean={result['latency_mean_us']:.1f}µs, p99={result['latency_p99_us']:.1f}µs"
                )
            else:
                print("ERROR")
        except Exception as e:
            print(f"FAILED: {e}")
            results["benchmarks"]["latency"][name] = {"error": str(e)}

    # 2. Throughput benchmarks
    print("\n[2/4] Running throughput benchmarks...")

    throughput_configs = [
        {
            "num_messages": 10000,
            "array_shape": (10,),
            "dtype": "float32",
            "name": "tiny_array",
        },
        {
            "num_messages": 5000,
            "array_shape": (100, 100),
            "dtype": "float32",
            "name": "small_array",
        },
        {
            "num_messages": 1000,
            "array_shape": (512, 512),
            "dtype": "float32",
            "name": "medium_array",
        },
        {
            "num_messages": 200,
            "array_shape": (1024, 1024),
            "dtype": "float32",
            "name": "large_array",
        },
    ]

    results["benchmarks"]["throughput"] = {}
    for config in throughput_configs:
        name = config.pop("name")
        print(f"  - {name}...", end=" ", flush=True)
        try:
            result = run_throughput_benchmark(**config)
            results["benchmarks"]["throughput"][name] = result
            if "error" not in result:
                print(
                    f"{result['throughput_msg_per_s']:.0f} msg/s, {result['throughput_mb_per_s']:.1f} MB/s"
                )
            else:
                print("ERROR")
        except Exception as e:
            print(f"FAILED: {e}")
            results["benchmarks"]["throughput"][name] = {"error": str(e)}

    # 3. Image-like data benchmarks
    print("\n[3/4] Running image data benchmarks...")

    image_configs = [
        {
            "num_messages": 1000,
            "array_shape": (480, 640, 3),
            "dtype": "uint8",
            "name": "vga_rgb",
        },
        {
            "num_messages": 500,
            "array_shape": (720, 1280, 3),
            "dtype": "uint8",
            "name": "720p_rgb",
        },
        {
            "num_messages": 200,
            "array_shape": (1080, 1920, 3),
            "dtype": "uint8",
            "name": "1080p_rgb",
        },
    ]

    results["benchmarks"]["images"] = {}
    for config in image_configs:
        name = config.pop("name")
        print(f"  - {name}...", end=" ", flush=True)
        try:
            result = run_throughput_benchmark(**config)
            results["benchmarks"]["images"][name] = result
            if "error" not in result:
                fps = result["throughput_msg_per_s"]
                mbps = result["throughput_mb_per_s"]
                print(f"{fps:.1f} fps, {mbps:.1f} MB/s")
            else:
                print("ERROR")
        except Exception as e:
            print(f"FAILED: {e}")
            results["benchmarks"]["images"][name] = {"error": str(e)}

    # 4. Serialization overhead
    print("\n[4/4] Measuring serialization overhead...")
    results["benchmarks"]["serialization"] = measure_serialization_overhead()

    return results


def get_system_info() -> dict:
    """Get system information."""
    import platform

    return {
        "platform": platform.system(),
        "platform_release": platform.release(),
        "processor": platform.processor(),
        "python_version": platform.python_version(),
        "numpy_version": np.__version__,
    }


def measure_serialization_overhead() -> dict:
    """Measure wire serialization overhead using multipart transport frames."""

    results = {}

    test_cases = [
        ("1KB_array", np.random.randn(256).astype(np.float32)),
        ("100KB_array", np.random.randn(256, 100).astype(np.float32)),
        ("1MB_array", np.random.randn(512, 512).astype(np.float32)),
        ("4MB_array", np.random.randn(1024, 1024).astype(np.float32)),
    ]

    for name, data in test_cases:
        message = ArrayMessage(data=data)
        topic = b"/benchmark/serialization"
        endpoint = f"inproc://cortex_serialization_{name}"
        context = zmq.Context.instance()
        sender = context.socket(zmq.PAIR)
        receiver = context.socket(zmq.PAIR)
        sender.setsockopt(zmq.LINGER, 0)
        receiver.setsockopt(zmq.LINGER, 0)
        receiver.bind(endpoint)
        sender.connect(endpoint)

        def frame_size_bytes(frames: list[object]) -> int:
            total = 0
            for frame in frames:
                if hasattr(frame, "nbytes"):
                    total += int(frame.nbytes)
                else:
                    total += len(frame)
            return total

        # Warm up
        for _ in range(10):
            sender.send_multipart([topic, *message.to_frames()], copy=False)
            warmup_frames = receiver.recv_multipart(copy=False)
            ArrayMessage.from_frames(warmup_frames[1:])

        # Benchmark A->B wire transfer and B-side decode
        iterations = 100
        wire_total = 0.0
        decode_total = 0.0
        frames = []

        for _ in range(iterations):
            wire_start = time.perf_counter()
            sender.send_multipart([topic, *message.to_frames()], copy=False)
            frames = receiver.recv_multipart(copy=False)
            wire_end = time.perf_counter()

            decode_start = wire_end
            ArrayMessage.from_frames(frames[1:])
            decode_end = time.perf_counter()

            wire_total += wire_end - wire_start
            decode_total += decode_end - decode_start

        serialize_time = (wire_total / iterations) * 1000  # ms (A->B wire path)
        deserialize_time = (decode_total / iterations) * 1000  # ms (B-side decode)

        # Use real wire bytes including topic frame.
        data_size_bytes = frame_size_bytes(frames)
        data_size_kb = data_size_bytes / 1024

        results[name] = {
            "data_size_kb": data_size_kb,
            "wire_size_bytes": data_size_bytes,
            "serialize_ms": serialize_time,
            "deserialize_ms": deserialize_time,
            "total_ms": serialize_time + deserialize_time,
            # Throughput is intentionally omitted here because inproc multipart
            # transport with copy=False can look unrealistically high and is
            # often misread as physical link bandwidth.
            "roundtrip_ms": serialize_time + deserialize_time,
        }

        print(
            f"  - {name}: to_wire={serialize_time:.3f}ms, from_wire={deserialize_time:.3f}ms"
        )

        sender.close()
        receiver.close()

    return results


def print_summary(results: dict) -> None:
    """Print a summary of all benchmark results."""

    print("\n" + "=" * 80)
    print("BENCHMARK SUMMARY")
    print("=" * 80)

    # Latency summary
    print("\n📊 LATENCY (microseconds)")
    print("-" * 60)
    print(f"{'Test':<20} {'Mean':>10} {'P50':>10} {'P99':>10} {'Max':>10}")
    print("-" * 60)

    for name, data in results["benchmarks"].get("latency", {}).items():
        if "error" not in data:
            print(
                f"{name:<20} {data['latency_mean_us']:>10.1f} "
                f"{data['latency_p50_us']:>10.1f} "
                f"{data['latency_p99_us']:>10.1f} "
                f"{data['latency_max_us']:>10.1f}"
            )

    # Throughput summary
    print("\n📊 THROUGHPUT")
    print("-" * 60)
    print(f"{'Test':<20} {'Msg/s':>12} {'MB/s':>10} {'Loss %':>10}")
    print("-" * 60)

    for name, data in results["benchmarks"].get("throughput", {}).items():
        if "error" not in data:
            print(
                f"{name:<20} {data['throughput_msg_per_s']:>12,.0f} "
                f"{data['throughput_mb_per_s']:>10.1f} "
                f"{data['loss_rate_percent']:>10.2f}"
            )

    # Image throughput
    print("\n📊 IMAGE DATA (frames per second)")
    print("-" * 60)
    print(f"{'Resolution':<20} {'FPS':>10} {'MB/s':>10} {'Loss %':>10}")
    print("-" * 60)

    for name, data in results["benchmarks"].get("images", {}).items():
        if "error" not in data:
            print(
                f"{name:<20} {data['throughput_msg_per_s']:>10.1f} "
                f"{data['throughput_mb_per_s']:>10.1f} "
                f"{data['loss_rate_percent']:>10.2f}"
            )

    # Wire serialization overhead
    print("\n📊 WIRE SERIALIZATION OVERHEAD (MULTIPART)")
    print("-" * 60)
    print(
        f"{'Size':<20} {'Wire Bytes':>12} {'To Wire':>12} {'From Wire':>12} {'Roundtrip':>12}"
    )
    print("-" * 60)

    for name, data in results["benchmarks"].get("serialization", {}).items():
        print(
            f"{name:<20} {data['wire_size_bytes']:>12,} "
            f"{data['serialize_ms']:>10.3f}ms "
            f"{data['deserialize_ms']:>10.3f}ms "
            f"{data['roundtrip_ms']:>10.3f}ms"
        )

    print("\n" + "=" * 80)


def main():
    parser = argparse.ArgumentParser(description="Cortex Benchmark Suite")
    parser.add_argument(
        "-o", "--output", type=str, default=None, help="Output file for JSON results"
    )
    parser.add_argument(
        "--quick",
        action="store_true",
        help="Run quick benchmarks with fewer iterations",
    )

    args = parser.parse_args()

    results = run_all_benchmarks()
    print_summary(results)

    if args.output:
        output_path = Path(args.output)
        with open(output_path, "w") as f:
            json.dump(results, f, indent=2, default=str)
        print(f"\nResults saved to: {output_path}")


if __name__ == "__main__":
    main()
