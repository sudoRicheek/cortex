#!/usr/bin/env python3
"""
Example: async subscriber for the same /cmd/wheel_velocity control topic.

Uses ``Node.create_subscriber`` (default ``mode='async'``) with an
``async def`` callback driven by the asyncio event loop. Pair with
either publisher example — the wire format is identical.

Compare the printed end-to-end latency to
``sync_subscriber_control_loop.py``:

* on stock CPython the async path adds ~3-4 ``await`` hops, so p99
  sits around 1-1.5 ms and the worst case stretches into many ms when
  the asyncio thread is busy
* the sync path stays bounded at hundreds of microseconds even under
  load

Usage:
    # Terminal 1
    python -m cortex.discovery.daemon

    # Terminal 2 (pick either)
    python examples/async_publisher_control_loop.py
    python examples/sync_publisher_control_loop.py

    # Terminal 3
    python examples/async_subscriber_control_loop.py
"""

import asyncio
import time
from dataclasses import dataclass

import cortex
from cortex import Message, Node


@dataclass
class WheelCommand(Message):
    """Identical layout to the publisher / sync examples."""

    left_rad_s: float
    right_rad_s: float
    issued_at_ns: int


class AsyncControlSubscriberNode(Node):
    """Subscribes to /cmd/wheel_velocity and prints rolling latency stats."""

    def __init__(self) -> None:
        super().__init__(name="async_control_sub")

        self.cmd_count = 0
        self.latency_sum_us = 0.0
        self.last_cmd: WheelCommand | None = None

        # Default mode is "async": one asyncio task drives recv → decode → cb.
        self.create_subscriber(
            topic_name="/cmd/wheel_velocity",
            message_type=WheelCommand,
            callback=self.on_wheel_command,  # async def, see below
            queue_size=64,
        )

        # An async telemetry timer running in the same node — exactly the
        # pattern the sync example shows, but here everything is async.
        self.create_timer(1 / 50.0, self.publish_telemetry)

        print("Subscribed to /cmd/wheel_velocity in async mode")
        print("Async telemetry running at 50 Hz")
        print("Press Ctrl+C to stop")

    async def on_wheel_command(self, msg: WheelCommand, header) -> None:
        """Async callback — runs as part of the asyncio event loop."""
        latency_us = (time.time_ns() - msg.issued_at_ns) / 1000.0
        self.last_cmd = msg
        self.cmd_count += 1
        self.latency_sum_us += latency_us

        if self.cmd_count % 100 == 0:
            avg_us = self.latency_sum_us / self.cmd_count
            print(
                f"[async] {self.cmd_count} cmds  "
                f"L={msg.left_rad_s:+.2f}  R={msg.right_rad_s:+.2f}  "
                f"latency last={latency_us:.0f}µs avg={avg_us:.0f}µs  "
                f"seq={header.sequence}"
            )

        # Yielding here is a no-op for this example, but real callbacks
        # often await downstream IO (DB, HTTP, another publish) — that's
        # the kind of work async mode composes well with.
        await asyncio.sleep(0)

    async def publish_telemetry(self) -> None:
        if self.last_cmd is None:
            return
        if self.cmd_count and self.cmd_count % 250 == 0:
            print(f"[async] heartbeat — {self.cmd_count} cmds applied so far")


async def main() -> None:
    print("Starting async control-loop subscriber...")
    async with AsyncControlSubscriberNode() as node:
        try:
            await node.run()
        except KeyboardInterrupt:
            print("\nShutting down...")


if __name__ == "__main__":
    cortex.run(main())
