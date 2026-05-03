#!/usr/bin/env python3
"""
Example: async publisher driving the same control topic as the sync example.

Uses ``Node.create_publisher`` (default ``mode='async'``) and a periodic
``Node.create_timer`` to publish wheel commands at 1 kHz through the
asyncio scheduler. Pair with either ``async_subscriber_control_loop.py``
or ``sync_subscriber_control_loop.py`` — they all share the same topic
and message, so any pub/sub combination works on the wire.

Compare the printed jitter against ``sync_publisher_control_loop.py``:
the asyncio loop's scheduling resolution is what limits how tightly we
can hit a high-rate target. On most machines this caps out around
~500-1000 Hz with notable variance; the sync example sits cleanly at
1 kHz on the same hardware.

Usage:
    # Terminal 1
    python -m cortex.discovery.daemon

    # Terminal 2
    python examples/async_publisher_control_loop.py

    # Terminal 3 (pick either)
    python examples/async_subscriber_control_loop.py
    python examples/sync_subscriber_control_loop.py
"""

import math
import time
from dataclasses import dataclass

import cortex
from cortex import Message, Node


@dataclass
class WheelCommand(Message):
    """Identical layout to the sync examples — the fingerprint must match."""

    left_rad_s: float
    right_rad_s: float
    issued_at_ns: int


class AsyncControlPublisherNode(Node):
    """Publishes /cmd/wheel_velocity through the asyncio scheduler."""

    def __init__(self, rate_hz: float = 1000.0) -> None:
        super().__init__(name="async_control_pub")

        self._rate_hz = rate_hz
        self._t0 = time.perf_counter()
        self._sent = 0

        # Default mode is "async": shares the node's zmq.asyncio.Context.
        self.pub = self.create_publisher(
            topic_name="/cmd/wheel_velocity",
            message_type=WheelCommand,
        )

        # Periodic timer scheduled by the asyncio event loop.
        self.create_timer(1.0 / rate_hz, self._tick)

        print(f"Publishing /cmd/wheel_velocity at ~{rate_hz:.0f} Hz (async)")
        print("Press Ctrl+C to stop")

    async def _tick(self) -> None:
        t = time.perf_counter() - self._t0
        left = 1.5 * math.sin(2 * math.pi * 0.25 * t)
        right = 1.5 * math.sin(2 * math.pi * 0.25 * t + math.pi / 8)

        # ``publish`` itself is sync (zmq.PUB.send_multipart) — only the
        # scheduling around it goes through asyncio.
        self.pub.publish(
            WheelCommand(
                left_rad_s=left,
                right_rad_s=right,
                issued_at_ns=time.time_ns(),
            )
        )
        self._sent += 1

        if self._sent % 1000 == 0:
            elapsed = time.perf_counter() - self._t0
            print(
                f"  sent {self._sent} cmds in {elapsed:.1f}s "
                f"(actual {self._sent / elapsed:.0f} Hz, target {self._rate_hz:.0f} Hz)"
            )


async def main() -> None:
    print("Starting async control-loop publisher...")
    async with AsyncControlPublisherNode() as node:
        try:
            await node.run()
        except KeyboardInterrupt:
            print("\nShutting down...")


if __name__ == "__main__":
    cortex.run(main())
