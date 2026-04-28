#!/usr/bin/env python3
"""
Example: Dictionary message subscriber.

This example demonstrates receiving complex nested dictionaries
with mixed types including NumPy arrays.

Usage:
    # Terminal 1: Start discovery daemon
    python -m cortex.discovery.daemon

    # Terminal 2: Start publisher
    python examples/publisher_dict.py

    # Terminal 3: Start subscriber
    python examples/subscriber_dict.py
"""

import asyncio
import contextlib

import cortex
from cortex import Node
from cortex.messages.base import MessageHeader
from cortex.messages.standard import DictMessage


class DictSubscriberNode(Node):
    """Node that subscribes to robot state dictionary messages."""

    def __init__(self) -> None:
        super().__init__(name="dict_subscriber")
        # Create a subscriber - connection happens asynchronously in run()
        print("Waiting for publisher on /robot/state...")
        self.create_subscriber(
            topic_name="/robot/state",
            message_type=DictMessage,
            callback=self._on_state_received,
            wait_for_topic=True,
            topic_timeout=30.0,
        )

        print("Subscriber created, will connect when run() is called...")
        print("Press Ctrl+C to stop")
        print()

    async def _on_state_received(self, msg: DictMessage, header: MessageHeader) -> None:
        """Callback for received state messages."""
        state = msg.data

        print(f"=== Robot State (seq={header.sequence}) ===")
        print(f"  Timestamp: {state['timestamp']:.3f}")

        pos = state["pose"]["position"]
        print(f"  Position: ({pos['x']:.3f}, {pos['y']:.3f}, {pos['z']:.3f})")

        ori = state["pose"]["orientation"]
        print(
            f"  Orientation: ({ori['x']:.3f}, {ori['y']:.3f}, "
            f"{ori['z']:.3f}, {ori['w']:.3f})"
        )

        vel = state["velocity"]
        print(f"  Linear Velocity: {vel['linear']}")
        print(f"  Angular Velocity: {vel['angular']}")

        print(f"  Joint Positions: {state['joint_positions']}")

        status = state["status"]
        print(
            f"  Status: moving={status['is_moving']}, "
            f"battery={status['battery_level']}%"
        )
        print()


async def main() -> None:
    """Run the dictionary subscriber example."""
    print("Starting dictionary subscriber...")

    node = DictSubscriberNode()

    try:
        await node.run()
    except asyncio.CancelledError:
        pass
    finally:
        await node.close()
        print("Shutting down...")


if __name__ == "__main__":
    with contextlib.suppress(KeyboardInterrupt):
        cortex.run(main())
