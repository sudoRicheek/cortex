#!/usr/bin/env python3
"""
Example: Multi-node system with custom messages.

This example demonstrates a more complete system with:
- Custom message types
- Multiple nodes communicating
- Timer-based publishing
- Using asyncio for concurrent node execution

Usage:
    # Terminal 1: Start discovery daemon
    python -m cortex.discovery.daemon

    # Terminal 2: Run the multi-node example
    python examples/multi_node_system.py
"""

import asyncio
import contextlib
import time
from dataclasses import dataclass

import numpy as np

import cortex
from cortex import Message, Node
from cortex.messages.base import MessageHeader


# Define custom message types
@dataclass
class SensorReading(Message):
    """Raw sensor reading from a sensor."""

    sensor_id: str
    timestamp: float
    values: np.ndarray  # Raw sensor values
    temperature: float  # Sensor temperature


@dataclass
class ProcessedData(Message):
    """Processed data after filtering."""

    source_sensor: str
    timestamp: float
    filtered_values: np.ndarray
    statistics: dict  # mean, std, min, max


@dataclass
class SystemStatus(Message):
    """Overall system status."""

    timestamp: float
    num_sensors: int
    processing_rate_hz: float
    total_messages: int


class SensorNode(Node):
    """Simulates a sensor that publishes raw readings."""

    def __init__(self, sensor_id: str, publish_rate: float = 10.0) -> None:
        super().__init__(f"sensor_{sensor_id}")
        self.sensor_id = sensor_id
        self._count = 0

        # Create publisher
        self.reading_pub = self.create_publisher(
            f"/sensor/{sensor_id}/raw", SensorReading
        )

        # Create timer for publishing
        self.create_timer(1.0 / publish_rate, self._publish_reading)

        print(f"[{self.name}] Initialized, publishing at {publish_rate} Hz")

    async def _publish_reading(self) -> None:
        """Publish a sensor reading."""
        # Simulate sensor data
        t = time.time()
        values = np.sin(np.linspace(0, 2 * np.pi, 100) + t) + np.random.randn(100) * 0.1

        msg = SensorReading(
            sensor_id=self.sensor_id,
            timestamp=t,
            values=values.astype(np.float32),
            temperature=25.0 + np.random.randn() * 0.5,
        )

        self.reading_pub.publish(msg)
        self._count += 1


class ProcessorNode(Node):
    """Processes raw sensor data and publishes filtered results."""

    def __init__(self, sensor_ids: list[str]) -> None:
        super().__init__("processor")
        self.sensor_ids = sensor_ids
        self._process_count = 0

        # Create subscribers for each sensor
        for sid in sensor_ids:
            self.create_subscriber(
                f"/sensor/{sid}/raw", SensorReading, callback=self._on_sensor_reading
            )

        # Create publisher for processed data
        self.processed_pub = self.create_publisher("/processed/data", ProcessedData)

        print(f"[{self.name}] Initialized, subscribing to {len(sensor_ids)} sensors")

    async def _on_sensor_reading(
        self, msg: SensorReading, header: MessageHeader
    ) -> None:
        """Process incoming sensor data."""
        # Simple low-pass filter
        kernel_size = 5
        kernel = np.ones(kernel_size) / kernel_size
        filtered = np.convolve(msg.values, kernel, mode="same")

        # Compute statistics
        stats = {
            "mean": float(np.mean(filtered)),
            "std": float(np.std(filtered)),
            "min": float(np.min(filtered)),
            "max": float(np.max(filtered)),
        }

        processed = ProcessedData(
            source_sensor=msg.sensor_id,
            timestamp=msg.timestamp,
            filtered_values=filtered.astype(np.float32),
            statistics=stats,
        )

        self.processed_pub.publish(processed)
        self._process_count += 1


class MonitorNode(Node):
    """Monitors the system and logs processed data."""

    def __init__(self) -> None:
        super().__init__("monitor")
        self._message_count = 0
        self._last_status_time = time.time()

        # Subscribe to processed data
        self.create_subscriber(
            "/processed/data", ProcessedData, callback=self._on_processed_data
        )

        # Publish system status
        self.status_pub = self.create_publisher("/system/status", SystemStatus)

        # Timer for status updates
        self.create_timer(1.0, self._publish_status)

        print(f"[{self.name}] Initialized")

    async def _on_processed_data(
        self, msg: ProcessedData, header: MessageHeader
    ) -> None:
        """Handle processed data."""
        self._message_count += 1

        # Log every 10th message
        if self._message_count % 10 == 0:
            stats = msg.statistics
            print(
                f"[{self.name}] Sensor {msg.source_sensor}: "
                f"mean={stats['mean']:.3f}, std={stats['std']:.3f}"
            )

    async def _publish_status(self) -> None:
        """Publish system status."""
        now = time.time()
        elapsed = now - self._last_status_time
        rate = self._message_count / elapsed if elapsed > 0 else 0

        status = SystemStatus(
            timestamp=now,
            num_sensors=2,  # We have 2 sensors in this example
            processing_rate_hz=rate,
            total_messages=self._message_count,
        )

        self.status_pub.publish(status)

        print(
            f"[{self.name}] System status: {self._message_count} messages, "
            f"{rate:.1f} Hz processing rate"
        )

        self._last_status_time = now
        self._message_count = 0


async def main() -> None:
    """Run the multi-node system."""
    print("Starting multi-node system example...")
    print("This demonstrates:")
    print("  - Custom message types")
    print("  - Multiple nodes with pub/sub")
    print("  - Timer-based publishing")
    print()

    # Create nodes
    sensor_ids = ["lidar", "camera"]

    sensor_nodes = [SensorNode(sid, publish_rate=10.0) for sid in sensor_ids]
    processor_node = ProcessorNode(sensor_ids)
    monitor_node = MonitorNode()

    all_nodes = [*sensor_nodes, processor_node, monitor_node]

    # Give time for connections
    await asyncio.sleep(1.0)

    print("\nSystem running. Press Ctrl+C to stop.\n")

    try:
        # Run all nodes concurrently
        await asyncio.gather(*[node.run() for node in all_nodes])
    except asyncio.CancelledError:
        pass
    finally:
        # Close all nodes
        for node in all_nodes:
            await node.close()
        print("System stopped.")


if __name__ == "__main__":
    with contextlib.suppress(KeyboardInterrupt):
        cortex.run(main())
