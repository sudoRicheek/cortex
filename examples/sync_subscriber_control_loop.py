#!/usr/bin/env python3
"""
Example: synchronous low-latency subscriber for a control topic.

Pairs with ``examples/sync_publisher_control_loop.py``. Demonstrates the
``mode='sync'`` opt-in: the receive loop runs on a dedicated OS thread
with a synchronous ``zmq.Poller``, the callback is a plain function (no
``await``), and ``queue_size=1`` gives latest-wins semantics suitable
for control commands.

The node also runs an async telemetry timer at 50 Hz so you can see the
two paths coexisting in the same process — sync for the hot loop, async
for everything else.

Usage:
    # Terminal 1
    python -m cortex.discovery.daemon

    # Terminal 2
    python examples/sync_publisher_control_loop.py

    # Terminal 3
    python examples/sync_subscriber_control_loop.py
"""

import time
from dataclasses import dataclass

import cortex
from cortex import Message, Node


@dataclass
class WheelCommand(Message):
    """Wheel-velocity command. Tiny payload — control-loop shape."""

    left_rad_s: float
    right_rad_s: float
    issued_at_ns: int


class ControlLoopNode(Node):
    """Subscribes to wheel commands on a sync thread and applies them.

    A real robot would forward each command to the motor driver. Here we
    just measure end-to-end latency from publish to callback so you can
    eyeball the difference between sync and async modes.
    """

    def __init__(self) -> None:
        super().__init__(name="control_loop_node")

        self.last_cmd: WheelCommand | None = None
        self.cmd_count = 0
        self.latency_sum_us = 0.0

        # ---- sync receive on a dedicated thread ------------------------
        self.create_subscriber(
            topic_name="/cmd/wheel_velocity",
            message_type=WheelCommand,
            callback=self.on_wheel_command,  # plain def, NOT async def
            mode="sync",
            queue_size=1,  # latest-wins
            # cpu_affinity=[3],               # uncomment on Linux to pin
            # sched_priority=20,              # uncomment if you have CAP_SYS_NICE
        )

        # ---- async telemetry timer in the same process ----------------
        self.create_timer(1 / 50.0, self.publish_telemetry)

        print("Subscribed to /cmd/wheel_velocity in sync mode")
        print("Async telemetry running at 50 Hz")
        print("Press Ctrl+C to stop")

    # callback runs on the sync receive thread
    def on_wheel_command(self, msg: WheelCommand, header) -> None:
        latency_us = (time.time_ns() - msg.issued_at_ns) / 1000.0
        self.last_cmd = msg
        self.cmd_count += 1
        self.latency_sum_us += latency_us

        if self.cmd_count % 100 == 0:
            avg_us = self.latency_sum_us / self.cmd_count
            print(
                f"[sync] {self.cmd_count} cmds  "
                f"L={msg.left_rad_s:+.2f}  R={msg.right_rad_s:+.2f}  "
                f"latency last={latency_us:.0f}µs avg={avg_us:.0f}µs  "
                f"seq={header.sequence}"
            )

    # async timer body — coexists happily with the sync subscriber
    async def publish_telemetry(self) -> None:
        if self.last_cmd is None:
            return
        # In a real node this would publish wheel encoder readings, IMU,
        # or whatever you want at lower-priority. We just print a heartbeat.
        if self.cmd_count and self.cmd_count % 250 == 0:
            print(f"[async] heartbeat — {self.cmd_count} cmds applied so far")


async def main() -> None:
    print("Starting sync-mode control-loop subscriber...")
    node = ControlLoopNode()
    try:
        await node.run()
    except KeyboardInterrupt:
        print("\nShutting down...")
    finally:
        await node.close()


if __name__ == "__main__":
    cortex.run(main())
