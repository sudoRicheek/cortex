#!/usr/bin/env python3
"""
Example: Simple NumPy array publisher using asyncio.

This example demonstrates publishing NumPy arrays using the Cortex framework.
Run this alongside subscriber_numpy.py to see the data transfer.

Usage:
    # Terminal 1: Start discovery daemon
    python -m cortex.discovery.daemon

    # Terminal 2: Start publisher
    python examples/publisher_numpy.py

    # Terminal 3: Start subscriber
    python examples/subscriber_numpy.py
"""

import numpy as np

import cortex
from cortex import ArrayMessage, Node


class ArrayPublisherNode(Node):
    """Example node that publishes NumPy arrays."""

    def __init__(self):
        super().__init__(name="array_publisher")

        # Create a publisher for array data
        self.pub = self.create_publisher(
            topic_name="/sensor/array_data",
            message_type=ArrayMessage,
        )

        # Create a timer to publish at 10 Hz
        self.count = 0
        self.create_timer(1 / 10, self.publish_array)

        print("Publishing on /sensor/array_data")
        print("Press Ctrl+C to stop")

    async def publish_array(self):
        """Publish array data at a constant rate."""
        # Generate some random sensor data
        # Simulating a 64x64 grayscale image
        data = np.random.randn(64, 64).astype(np.float32)

        # Add some structure (moving gradient)
        x = np.linspace(0, 2 * np.pi, 64)
        y = np.linspace(0, 2 * np.pi, 64)
        X, Y = np.meshgrid(x, y)
        data += np.sin(X + self.count * 0.1) * np.cos(Y + self.count * 0.1)

        # Create and publish message
        msg = ArrayMessage(
            data=data, name=f"frame_{self.count}", frame_id="sensor_frame"
        )

        self.pub.publish(msg)

        if self.count % 10 == 0:
            print(
                f"Published frame {self.count}, shape={data.shape}, mean={data.mean():.3f}"
            )

        self.count += 1


async def main():
    """Run the publisher example."""
    print("Starting NumPy array publisher...")

    node = ArrayPublisherNode()

    try:
        await node.run()
    except KeyboardInterrupt:
        print("\nShutting down...")
    finally:
        await node.close()


if __name__ == "__main__":
    cortex.run(main())
