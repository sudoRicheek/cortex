#!/usr/bin/env python3
"""
Example: Dictionary message publisher.

This example demonstrates publishing complex nested dictionaries
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
import time

import numpy as np

import cortex
from cortex import Node
from cortex.messages.standard import DictMessage


class DictPublisherNode(Node):
    """Node that publishes robot state as dictionary messages."""

    def __init__(self) -> None:
        super().__init__(name="dict_publisher")
        self._count = 0
        self._pub = self.create_publisher(
            topic_name="/robot/state",
            message_type=DictMessage,
        )
        # 5 Hz publishing rate
        self.create_timer(1.0 / 5.0, self._publish_state)
        print("Publishing on /robot/state at 5 Hz")
        print("Press Ctrl+C to stop")

    async def _publish_state(self) -> None:
        """Publish robot state data."""
        state = {
            "timestamp": time.time(),
            "frame_id": "base_link",
            "pose": {
                "position": {
                    "x": np.sin(self._count * 0.1) * 2.0,
                    "y": np.cos(self._count * 0.1) * 2.0,
                    "z": 0.0,
                },
                "orientation": {
                    "x": 0.0,
                    "y": 0.0,
                    "z": np.sin(self._count * 0.05),
                    "w": np.cos(self._count * 0.05),
                },
            },
            "velocity": {
                "linear": np.array([0.5, 0.0, 0.0], dtype=np.float32),
                "angular": np.array([0.0, 0.0, 0.1], dtype=np.float32),
            },
            "joint_positions": np.random.randn(7).astype(np.float32),
            "joint_names": [
                "joint_1",
                "joint_2",
                "joint_3",
                "joint_4",
                "joint_5",
                "joint_6",
                "joint_7",
            ],
            "status": {
                "is_moving": True,
                "battery_level": 85.5,
                "error_code": 0,
            },
        }

        msg = DictMessage(data=state)
        self._pub.publish(msg)

        if self._count % 5 == 0:
            pos = state["pose"]["position"]
            print(
                f"Published state {self._count}: pos=({pos['x']:.2f}, {pos['y']:.2f})"
            )

        self._count += 1


async def main() -> None:
    """Run the dictionary publisher example."""
    print("Starting dictionary publisher...")

    node = DictPublisherNode()

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
