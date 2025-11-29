#!/usr/bin/env python3
"""
Example: PyTorch tensor publisher using asyncio.

This example demonstrates publishing PyTorch tensors using the Cortex framework.
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
from cortex import Node, TensorMessage


class TensorPublisherNode(Node):
    """Example node that publishes PyTorch tensors."""

    def __init__(self):
        super().__init__(name="tensor_publisher")

        # Create a publisher
        self.pub = self.create_publisher(
            topic_name="/model/features",
            message_type=TensorMessage,
        )

        # Create a timer to publish at 10 Hz
        self.count = 0
        self.create_timer(1 / 10, self.publish_tensor)

        print("Publishing on /model/features")
        print("Press Ctrl+C to stop")

    async def publish_tensor(self):
        """Publish tensor data at a constant rate."""
        # Simulate model output (e.g., image features)
        # Batch of 4, 256 feature channels, 7x7 spatial
        features = torch.randn(4, 256, 7, 7)

        # Add some structure
        features = features + torch.sin(torch.tensor(self.count * 0.1))

        msg = TensorMessage(data=features, name=f"features_batch_{self.count}")

        self.pub.publish(msg)

        if self.count % 10 == 0:
            print(
                f"Published tensor {self.count}: shape={tuple(features.shape)}, "
                f"mean={features.mean():.4f}, std={features.std():.4f}"
            )

        self.count += 1


async def main():
    """Run the tensor publisher example."""
    print("Starting PyTorch tensor publisher...")
    print(f"PyTorch version: {torch.__version__}")

    node = TensorPublisherNode()

    try:
        await node.run()
    except KeyboardInterrupt:
        print("\nShutting down...")
    finally:
        await node.close()


if __name__ == "__main__":
    cortex.run(main())
