#!/usr/bin/env python3
"""
Example: pure-sync publisher driving a tight control loop.

Demonstrates that a node which owns only sync work needs no asyncio at
all: no ``async def``, no ``await``, no ``cortex.run`` — just plain
Python. The node provides ``spawn_thread`` for tracked sync workers and
``spin`` / ``close_sync`` for the lifecycle.

Pairs with ``examples/sync_subscriber_control_loop.py``.

Usage:
    # Terminal 1
    python -m cortex.discovery.daemon

    # Terminal 2
    python examples/sync_publisher_control_loop.py

    # Terminal 3
    python examples/sync_subscriber_control_loop.py
"""

import math
import threading
import time
from dataclasses import dataclass

from cortex import Message, Node


@dataclass
class WheelCommand(Message):
    """Must match the dataclass in sync_subscriber_control_loop.py exactly
    so the fingerprint is identical."""

    left_rad_s: float
    right_rad_s: float
    issued_at_ns: int


def control_loop(stop: threading.Event, pub, rate_hz: float) -> None:
    """Publish a sinusoidal wheel command at ``rate_hz`` until ``stop`` fires.

    The first argument is supplied by ``Node.spawn_thread`` — the node's
    shared stop event. Polling it on every iteration is what makes the
    thread shut down promptly when the node closes.
    """
    interval = 1.0 / rate_hz
    next_t = time.perf_counter()
    sent = 0
    t0 = time.perf_counter()

    print(f"Publishing /cmd/wheel_velocity at {rate_hz:.0f} Hz from a sync thread")

    while not stop.is_set():
        next_t += interval
        sleep = next_t - time.perf_counter()
        if sleep > 0:
            # Cap at 50 ms so we stay responsive to ``stop`` even if the
            # caller drops the rate way below 20 Hz.
            time.sleep(min(sleep, 0.05))

        t = time.perf_counter() - t0
        left = 1.5 * math.sin(2 * math.pi * 0.25 * t)
        right = 1.5 * math.sin(2 * math.pi * 0.25 * t + math.pi / 8)
        pub.publish(
            WheelCommand(
                left_rad_s=left,
                right_rad_s=right,
                issued_at_ns=time.time_ns(),
            )
        )
        sent += 1
        if sent % 1000 == 0:
            elapsed = time.perf_counter() - t0
            print(f"  sent {sent} cmds in {elapsed:.1f}s ({sent / elapsed:.0f} Hz)")


def main() -> None:
    print("Starting sync-mode control-loop publisher...")
    with Node(name="control_loop_publisher") as node:
        pub = node.create_publisher(
            topic_name="/cmd/wheel_velocity",
            message_type=WheelCommand,
            mode="sync",  # independent zmq.Context, no asyncio shadow
        )

        # The node now owns the publisher thread: it gets the shared stop
        # event, ``spin()`` stays alive while it's running, and the
        # context manager joins it on exit.
        node.spawn_thread(control_loop, pub, 1000.0, name="cmd-publisher")

        try:
            node.spin()  # blocks until Ctrl+C or node.stop()
        except KeyboardInterrupt:
            print("\nShutting down...")


if __name__ == "__main__":
    main()
