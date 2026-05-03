#!/usr/bin/env python3
"""
Example: PyTorch tensor subscriber using asyncio.

This example demonstrates receiving PyTorch tensors using the Cortex framework.
Requires PyTorch to be installed.

Usage:
    # Terminal 1: Start discovery daemon
    python -m cortex.discovery.daemon

    # Terminal 2: Start publisher
    python examples/publisher_tensor.py

    # Terminal 3: Start subscriber
    python examples/subscriber_tensor.py
"""

try:
    import torch
except ImportError:
    print("This example requires PyTorch. Install with: pip install torch")
    exit(1)

import cortex
from cortex import Node
from cortex.messages.base import MessageHeader
from cortex.messages.standard import TensorMessage


async def on_tensor_received(msg: TensorMessage, header: MessageHeader):
    """Async callback for received tensor messages."""
    tensor = msg.data

    print(f"Received: {msg.name}")
    print(f"  Shape: {tuple(tensor.shape)}")
    print(f"  Dtype: {tensor.dtype}")
    print(f"  Device: {tensor.device}")
    print(f"  Mean: {tensor.mean():.4f}")
    print(f"  Std: {tensor.std():.4f}")
    print(f"  Min: {tensor.min():.4f}")
    print(f"  Max: {tensor.max():.4f}")
    print(f"  Sequence: {header.sequence}")
    print()


class TensorSubscriberNode(Node):
    """Example node that subscribes to PyTorch tensors."""

    def __init__(self):
        super().__init__(name="tensor_subscriber")

        # Create a subscriber - connection happens asynchronously in run()
        print("Waiting for publisher on /model/features...")
        self.sub = self.create_subscriber(
            topic_name="/model/features",
            message_type=TensorMessage,
            callback=on_tensor_received,
            wait_for_topic=True,
            topic_timeout=30.0,
        )

        print("Subscriber created, will connect when run() is called...")
        print("Press Ctrl+C to stop")
        print()


async def main():
    """Run the tensor subscriber example."""
    print("Starting PyTorch tensor subscriber...")
    print(f"PyTorch version: {torch.__version__}")

    node = TensorSubscriberNode()

    try:
        await node.run()
    except KeyboardInterrupt:
        print("\nShutting down...")
    finally:
        await node.close()


if __name__ == "__main__":
    cortex.run(main())
