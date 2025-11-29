"""
Tests for Node class.
"""

import asyncio
import contextlib
import time
from dataclasses import dataclass

import pytest

from cortex.core.node import Node
from cortex.messages.base import Message, MessageHeader
from cortex.messages.standard import IntMessage


@dataclass
class SensorData(Message):
    """Test sensor data message."""

    timestamp: float
    value: float
    sensor_id: str


class TestNode:
    """Tests for Node class."""

    @pytest.mark.asyncio
    async def test_node_creation(self, discovery_daemon, discovery_address):
        """Node should create successfully."""
        node = Node(name="test_node", discovery_address=discovery_address)

        assert node.name == "test_node"
        assert len(node.publishers) == 0
        assert len(node.subscribers) == 0

        await node.close()

    @pytest.mark.asyncio
    async def test_create_publisher(self, discovery_daemon, discovery_address):
        """Node should create publishers."""
        node = Node(name="pub_node", discovery_address=discovery_address)

        pub = node.create_publisher(
            topic_name="/sensor/data",
            message_type=SensorData,
        )

        assert pub is not None
        assert "/sensor/data" in node.publishers

        # Duplicate should return same publisher
        pub2 = node.create_publisher(
            topic_name="/sensor/data",
            message_type=SensorData,
        )
        assert pub is pub2

        await node.close()

    @pytest.mark.asyncio
    async def test_create_subscriber(self, discovery_daemon, discovery_address):
        """Node should create subscribers."""
        # First create a publisher
        pub_node = Node(name="pub_node", discovery_address=discovery_address)
        pub_node.create_publisher("/test/sub", SensorData)
        await asyncio.sleep(0.2)

        # Now create subscriber
        sub_node = Node(name="sub_node", discovery_address=discovery_address)

        received: list[SensorData] = []

        async def callback(msg: SensorData, header: MessageHeader) -> None:
            received.append(msg)

        sub = sub_node.create_subscriber(
            topic_name="/test/sub",
            message_type=SensorData,
            callback=callback,
        )

        assert sub is not None
        assert "/test/sub" in sub_node.subscribers

        await pub_node.close()
        await sub_node.close()

    @pytest.mark.asyncio
    async def test_node_pubsub_communication(self, discovery_daemon, discovery_address):
        """Nodes should communicate via pub/sub."""
        # Publisher node
        pub_node = Node(name="sensor_node", discovery_address=discovery_address)
        pub = pub_node.create_publisher("/sensor/data", SensorData)
        await asyncio.sleep(0.2)

        # Subscriber node
        sub_node = Node(name="processor_node", discovery_address=discovery_address)

        received: list[SensorData] = []
        event = asyncio.Event()

        async def callback(msg: SensorData, header: MessageHeader) -> None:
            received.append(msg)
            event.set()

        sub_node.create_subscriber("/sensor/data", SensorData, callback)
        await asyncio.sleep(0.2)

        # Start subscriber in background
        run_task = asyncio.create_task(sub_node.run())

        # Publish data
        await pub.publish(
            SensorData(timestamp=time.time(), value=42.0, sensor_id="sensor_1")
        )

        # Wait for message to be received
        with contextlib.suppress(asyncio.TimeoutError):
            await asyncio.wait_for(event.wait(), timeout=2.0)

        # Cancel the run task
        run_task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await run_task

        assert len(received) == 1
        assert received[0].value == 42.0
        assert received[0].sensor_id == "sensor_1"

        await pub_node.close()
        await sub_node.close()

    @pytest.mark.asyncio
    async def test_node_timer(self, discovery_daemon, discovery_address):
        """Node timers should fire."""
        node = Node(name="timer_node", discovery_address=discovery_address)

        fired: list[float] = []

        async def timer_callback() -> None:
            fired.append(time.time())

        node.create_timer(0.1, timer_callback)  # 10 Hz

        # Run node for a short time
        run_task = asyncio.create_task(node.run())

        # Wait for timer to fire multiple times
        await asyncio.sleep(0.35)

        # Cancel and clean up
        run_task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await run_task

        await node.close()

        assert len(fired) >= 2  # Should fire at least 2-3 times in 350ms

    @pytest.mark.asyncio
    async def test_node_spin_once(self, discovery_daemon, discovery_address):
        """Node should receive messages via async run."""
        # Setup
        pub_node = Node(name="pub", discovery_address=discovery_address)
        pub = pub_node.create_publisher("/spin_test", IntMessage)
        await asyncio.sleep(0.2)

        sub_node = Node(name="sub", discovery_address=discovery_address)
        received: list[IntMessage] = []
        event = asyncio.Event()

        async def callback(msg: IntMessage, header: MessageHeader) -> None:
            received.append(msg)
            event.set()

        sub_node.create_subscriber("/spin_test", IntMessage, callback)
        await asyncio.sleep(0.2)

        # Start subscriber
        run_task = asyncio.create_task(sub_node.run())

        # Publish
        await pub.publish(IntMessage(data=123))

        # Wait for message
        with contextlib.suppress(asyncio.TimeoutError):
            await asyncio.wait_for(event.wait(), timeout=2.0)

        run_task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await run_task

        assert len(received) == 1
        assert received[0].data == 123

        await pub_node.close()
        await sub_node.close()


class TestNodeLifecycle:
    """Tests for node lifecycle management."""

    @pytest.mark.asyncio
    async def test_node_stop(self, discovery_daemon, discovery_address):
        """Node stop should work correctly via task cancellation."""
        node = Node(name="stop_test", discovery_address=discovery_address)

        # Start running
        run_task = asyncio.create_task(node.run())

        await asyncio.sleep(0.1)

        # Cancel the task (equivalent to stop)
        run_task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await run_task

        await node.close()

    @pytest.mark.asyncio
    async def test_node_destroy_cleans_up(self, discovery_daemon, discovery_address):
        """Node close should clean up resources."""
        node = Node(name="destroy_test", discovery_address=discovery_address)

        # Create some publishers
        node.create_publisher("/test1", IntMessage)
        node.create_publisher("/test2", IntMessage)

        assert len(node.publishers) == 2

        # Close
        await node.close()

        assert len(node.publishers) == 0
        assert len(node.subscribers) == 0

    @pytest.mark.asyncio
    async def test_node_async_context_manager(
        self, discovery_daemon, discovery_address
    ):
        """Node should work as async context manager."""
        async with Node(
            name="context_node", discovery_address=discovery_address
        ) as node:
            pub = node.create_publisher("/context_test", IntMessage)
            assert pub is not None

        # After exiting, node should be closed
        # (hard to verify, but at least no exceptions)
