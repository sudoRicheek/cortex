#!/usr/bin/env python3
"""
Example: pure-sync publisher driving a tight control loop.

A node which owns only sync work needs no asyncio at all.
``Node.create_timer(..., mode='sync')`` spawns a dedicated OS thread
that drives the callback at the requested rate, and the node's
``__exit__`` joins it on shutdown.

Compare with ``async_publisher_control_loop.py``.

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


def main() -> None:
    rate_hz = 1000.0
    print(f"Publishing /cmd/wheel_velocity at {rate_hz:.0f} Hz from a sync timer")

    with Node(name="control_loop_publisher") as node:
        pub = node.create_publisher(
            topic_name="/cmd/wheel_velocity",
            message_type=WheelCommand,
            mode="sync",  # independent zmq.Context, no asyncio shadow
        )

        state = {"sent": 0, "t0": time.perf_counter()}

        def tick() -> None:
            t = time.perf_counter() - state["t0"]
            left = 1.5 * math.sin(2 * math.pi * 0.25 * t)
            right = 1.5 * math.sin(2 * math.pi * 0.25 * t + math.pi / 8)
            pub.publish(
                WheelCommand(
                    left_rad_s=left,
                    right_rad_s=right,
                    issued_at_ns=time.time_ns(),
                )
            )
            state["sent"] += 1
            if state["sent"] % 1000 == 0:
                elapsed = time.perf_counter() - state["t0"]
                print(
                    f"  sent {state['sent']} cmds in {elapsed:.1f}s "
                    f"({state['sent'] / elapsed:.0f} Hz)"
                )

        node.create_timer(1.0 / rate_hz, tick, mode="sync")

        try:
            node.spin()  # blocks until Ctrl+C or node.stop()
        except KeyboardInterrupt:
            print("\nShutting down...")


if __name__ == "__main__":
    main()
