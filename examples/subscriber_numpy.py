#!/usr/bin/env python3
"""
Example: Simple NumPy array subscriber using asyncio.

This example demonstrates receiving NumPy arrays using the Cortex framework.
Run this alongside publisher_numpy.py to see the data transfer.

Usage:
    # Terminal 1: Start discovery daemon
    python -m cortex.discovery.daemon

    # Terminal 2: Start publisher
    python examples/publisher_numpy.py

    # Terminal 3: Start subscriber
    python examples/subscriber_numpy.py
"""

import asyncio

from cortex import ArrayMessage, Node
from cortex.messages.base import MessageHeader


async def on_array_received(msg: ArrayMessage, header: MessageHeader):
    """Async callback for received array messages."""
    print(f"Received: {msg.name}")
    print(f"  Shape: {msg.data.shape}")
    print(f"  Dtype: {msg.data.dtype}")
    print(f"  Mean: {msg.data.mean():.4f}")
    print(f"  Std: {msg.data.std():.4f}")
    print(f"  Frame ID: {msg.frame_id}")
    print(f"  Timestamp: {header.timestamp_ns}")
    print(f"  Sequence: {header.sequence}")
    print()


class ArraySubscriberNode(Node):
    """Example node that subscribes to NumPy arrays."""

    def __init__(self):
        super().__init__(name="array_subscriber")

        # Create a subscriber
        print("Waiting for publisher on /sensor/array_data...")
        self.sub = self.create_subscriber(
            topic_name="/sensor/array_data",
            message_type=ArrayMessage,
            callback=on_array_received,
            wait_for_topic=True,
            topic_timeout=30.0,
        )

        if not self.sub.is_connected:
            raise RuntimeError("Failed to connect to topic!")

        print("Connected! Receiving messages...")
        print("Press Ctrl+C to stop")
        print()


async def main():
    """Run the subscriber example."""
    print("Starting NumPy array subscriber...")

    try:
        node = ArraySubscriberNode()
        await node.run()
    except RuntimeError as e:
        print(f"Error: {e}")
    except KeyboardInterrupt:
        print("\nShutting down...")
    finally:
        if "node" in locals():
            await node.close()


if __name__ == "__main__":
    asyncio.run(main())
