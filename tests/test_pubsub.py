"""
Tests for publisher and subscriber.
"""

import asyncio
import contextlib
import time
from dataclasses import dataclass

import numpy as np
import pytest

from cortex.core.publisher import Publisher
from cortex.core.subscriber import Subscriber
from cortex.messages.base import Message, MessageHeader
from cortex.messages.standard import ArrayMessage, DictMessage, StringMessage


@dataclass
class SampleMessage(Message):
    """Simple test message."""

    value: int
    name: str


class TestPublisher:
    """Tests for Publisher class."""

    def test_publisher_creates_socket(
        self, discovery_daemon, discovery_address, tmp_path
    ):
        """Publisher should create IPC socket."""
        pub = Publisher(
            topic_name="/test/publisher",
            message_type=SampleMessage,
            node_name="test_node",
            discovery_address=discovery_address,
        )

        assert pub.address.startswith("ipc://")

        pub.close()

    def test_publisher_registers_with_discovery(
        self, discovery_daemon, discovery_address
    ):
        """Publisher should register with discovery daemon."""
        pub = Publisher(
            topic_name="/test/registered",
            message_type=SampleMessage,
            node_name="test_node",
            discovery_address=discovery_address,
        )

        # Give time for registration
        time.sleep(0.2)

        assert pub.is_registered

        pub.close()

    def test_publisher_publishes_messages(self, discovery_daemon, discovery_address):
        """Publisher should publish messages."""
        pub = Publisher(
            topic_name="/test/publish",
            message_type=SampleMessage,
            node_name="test_node",
            discovery_address=discovery_address,
        )

        msg = SampleMessage(value=42, name="test")
        success = pub.publish(msg)

        assert success
        assert pub.publish_count == 1

        pub.close()

    def test_per_publisher_sequence_counter(self, discovery_daemon, discovery_address):
        """Two publishers of the same message type must not interleave sequences.

        Each Publisher owns its own counter; the counter starts at 0 and
        advances by one per successful publish, independently of any other
        Publisher in the process.
        """
        pub_a = Publisher(
            topic_name="/test/seq_a",
            message_type=SampleMessage,
            node_name="seq_node_a",
            discovery_address=discovery_address,
        )
        pub_b = Publisher(
            topic_name="/test/seq_b",
            message_type=SampleMessage,
            node_name="seq_node_b",
            discovery_address=discovery_address,
        )

        for i in range(5):
            assert pub_a.publish(SampleMessage(value=i, name="a"))
        for i in range(3):
            assert pub_b.publish(SampleMessage(value=i, name="b"))

        # ``_sequence`` is the next-to-emit value, so after N publishes it == N.
        assert pub_a._sequence == 5
        assert pub_b._sequence == 3

        pub_a.close()
        pub_b.close()

    def test_publisher_type_checking(self, discovery_daemon, discovery_address):
        """Publisher should reject wrong message types."""
        pub = Publisher(
            topic_name="/test/typecheck",
            message_type=SampleMessage,
            node_name="test_node",
            discovery_address=discovery_address,
        )

        wrong_msg = StringMessage(data="wrong type")

        with pytest.raises(TypeError):
            pub.publish(wrong_msg)

        pub.close()


class TestSubscriber:
    """Tests for Subscriber class."""

    def test_subscriber_connects_to_publisher(
        self, discovery_daemon, discovery_address
    ):
        """Subscriber should connect to existing publisher."""
        # Create publisher first
        pub = Publisher(
            topic_name="/test/sub_connect",
            message_type=SampleMessage,
            node_name="pub_node",
            discovery_address=discovery_address,
        )
        time.sleep(0.2)  # Let it register

        # Create subscriber
        sub = Subscriber(
            topic_name="/test/sub_connect",
            message_type=SampleMessage,
            node_name="sub_node",
            discovery_address=discovery_address,
            wait_for_topic=True,
            topic_timeout=5.0,
        )

        assert sub.is_connected
        assert sub.topic_info is not None
        assert sub.topic_info.address == pub.address

        sub.close()
        pub.close()

    @pytest.mark.asyncio
    async def test_subscriber_receives_messages(
        self, discovery_daemon, discovery_address
    ):
        """Subscriber should receive published messages."""
        # Create publisher
        pub = Publisher(
            topic_name="/test/sub_recv",
            message_type=SampleMessage,
            node_name="pub_node",
            discovery_address=discovery_address,
        )
        await asyncio.sleep(0.2)

        # Create subscriber
        received: list[SampleMessage] = []
        event = asyncio.Event()

        async def callback(msg: SampleMessage, header: MessageHeader) -> None:
            received.append(msg)
            event.set()

        sub = Subscriber(
            topic_name="/test/sub_recv",
            message_type=SampleMessage,
            callback=callback,
            node_name="sub_node",
            discovery_address=discovery_address,
        )

        # Need small delay for ZMQ connection
        await asyncio.sleep(0.2)

        # Start subscriber in background
        run_task = asyncio.create_task(sub.run())

        # Publish message
        pub.publish(SampleMessage(value=42, name="test"))

        # Wait for message to be received
        with contextlib.suppress(asyncio.TimeoutError):
            await asyncio.wait_for(event.wait(), timeout=2.0)

        # Stop subscriber
        sub.stop()
        run_task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await run_task

        assert len(received) == 1
        assert received[0].value == 42
        assert received[0].name == "test"

        sub.close()
        pub.close()

    @pytest.mark.asyncio
    async def test_subscriber_waits_for_topic(
        self, discovery_daemon, discovery_address
    ):
        """Subscriber should wait for topic to appear."""
        connected_event = asyncio.Event()

        async def subscriber_task():
            sub = Subscriber(
                topic_name="/test/wait_topic",
                message_type=SampleMessage,
                node_name="sub_node",
                discovery_address=discovery_address,
                wait_for_topic=True,
                topic_timeout=5.0,
            )
            # _async_connect is called in receive(), which waits for the topic
            await sub.receive()  # This triggers async wait
            connected_event.set()
            sub.close()

        # Start subscriber task (will wait for topic)
        sub_task = asyncio.create_task(subscriber_task())

        # Create publisher after delay
        await asyncio.sleep(0.5)
        pub = Publisher(
            topic_name="/test/wait_topic",
            message_type=SampleMessage,
            node_name="pub_node",
            discovery_address=discovery_address,
        )

        # Wait for subscriber to connect
        try:
            await asyncio.wait_for(connected_event.wait(), timeout=6.0)
            assert connected_event.is_set()
        finally:
            sub_task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await sub_task
            pub.close()


class TestPubSubIntegration:
    """Integration tests for pub/sub."""

    @pytest.mark.asyncio
    async def test_multiple_messages(self, discovery_daemon, discovery_address):
        """Multiple messages should be received in order."""
        pub = Publisher(
            topic_name="/test/multi_msg",
            message_type=SampleMessage,
            node_name="pub_node",
            discovery_address=discovery_address,
        )
        await asyncio.sleep(0.2)

        received: list[int] = []
        done_event = asyncio.Event()

        async def callback(msg: SampleMessage, header: MessageHeader) -> None:
            received.append(msg.value)
            if len(received) >= 10:
                done_event.set()

        sub = Subscriber(
            topic_name="/test/multi_msg",
            message_type=SampleMessage,
            callback=callback,
            node_name="sub_node",
            discovery_address=discovery_address,
        )
        await asyncio.sleep(0.2)

        # Start subscriber in background
        run_task = asyncio.create_task(sub.run())

        # Publish multiple messages
        for i in range(10):
            pub.publish(SampleMessage(value=i, name=f"msg_{i}"))

        # Wait for all messages
        with contextlib.suppress(asyncio.TimeoutError):
            await asyncio.wait_for(done_event.wait(), timeout=5.0)

        # Stop subscriber
        sub.stop()
        run_task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await run_task

        assert len(received) == 10
        assert received == list(range(10))

        sub.close()
        pub.close()

    @pytest.mark.asyncio
    async def test_numpy_array_transfer(self, discovery_daemon, discovery_address):
        """NumPy arrays should transfer correctly."""
        pub = Publisher(
            topic_name="/test/numpy",
            message_type=ArrayMessage,
            node_name="pub_node",
            discovery_address=discovery_address,
        )
        await asyncio.sleep(0.2)

        received: list[np.ndarray] = []
        event = asyncio.Event()

        async def callback(msg: ArrayMessage, header: MessageHeader) -> None:
            received.append(msg.data.copy())
            event.set()

        sub = Subscriber(
            topic_name="/test/numpy",
            message_type=ArrayMessage,
            callback=callback,
            node_name="sub_node",
            discovery_address=discovery_address,
        )
        await asyncio.sleep(0.2)

        # Start subscriber
        run_task = asyncio.create_task(sub.run())

        # Send large array
        arr = np.random.randn(100, 100).astype(np.float32)
        pub.publish(ArrayMessage(data=arr))

        # Wait for message
        with contextlib.suppress(asyncio.TimeoutError):
            await asyncio.wait_for(event.wait(), timeout=2.0)

        # Stop
        sub.stop()
        run_task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await run_task

        assert len(received) == 1
        np.testing.assert_array_almost_equal(arr, received[0])

        sub.close()
        pub.close()

    @pytest.mark.asyncio
    async def test_dict_message_transfer(self, discovery_daemon, discovery_address):
        """Dict messages should transfer correctly."""
        pub = Publisher(
            topic_name="/test/dict",
            message_type=DictMessage,
            node_name="pub_node",
            discovery_address=discovery_address,
        )
        await asyncio.sleep(0.2)

        received: list[dict] = []
        event = asyncio.Event()

        async def callback(msg: DictMessage, header: MessageHeader) -> None:
            received.append(msg.data)
            event.set()

        sub = Subscriber(
            topic_name="/test/dict",
            message_type=DictMessage,
            callback=callback,
            node_name="sub_node",
            discovery_address=discovery_address,
        )
        await asyncio.sleep(0.2)

        # Start subscriber
        run_task = asyncio.create_task(sub.run())

        # Send dict with nested structure
        data = {
            "name": "test",
            "values": [1, 2, 3],
            "nested": {"a": 1, "b": 2},
            "array": np.array([1.0, 2.0, 3.0]),
        }
        pub.publish(DictMessage(data=data))

        # Wait for message
        with contextlib.suppress(asyncio.TimeoutError):
            await asyncio.wait_for(event.wait(), timeout=2.0)

        # Stop
        sub.stop()
        run_task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await run_task

        assert len(received) == 1
        assert received[0]["name"] == "test"
        assert received[0]["values"] == [1, 2, 3]
        assert received[0]["nested"]["a"] == 1
        np.testing.assert_array_almost_equal(data["array"], received[0]["array"])

        sub.close()
        pub.close()
