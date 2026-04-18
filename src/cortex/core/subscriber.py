"""
Subscriber implementation for Cortex.

Provides a ZeroMQ-based subscriber that queries the discovery daemon
and subscribes to topics using IPC sockets with asyncio.
"""

import asyncio
import contextlib
import logging
import time
from typing import Any

import zmq
import zmq.asyncio

from cortex.core.executor import AsyncExecutor
from cortex.core.types import MessageCallback
from cortex.discovery.client import DiscoveryClient
from cortex.discovery.daemon import DEFAULT_DISCOVERY_ADDRESS
from cortex.discovery.protocol import TopicInfo
from cortex.messages.base import Message, MessageHeader

logger = logging.getLogger("cortex.subscriber")


class Subscriber:
    """Receives typed messages on a topic from a ZMQ SUB socket.

    On construction, the subscriber performs a non-blocking lookup against
    the discovery daemon. If the topic already has a publisher it connects
    immediately; otherwise it defers and retries with a polling wait inside
    :meth:`run`.

    When constructed with a ``callback`` the subscriber drives its own
    receive loop (one task, one callback at a time — see
    :class:`cortex.core.executor.AsyncExecutor`). Without a callback the
    subscriber is passive and the caller polls via :meth:`receive`.

    Always create via :meth:`Node.create_subscriber`.

    Example:
        ```python
        async def callback(msg, header):
            print(f"Received: {msg}")

        async with Node("my_node") as node:
            node.create_subscriber("/topic", MyMsg, callback)
            await node.run()
        ```
    """

    def __init__(
        self,
        topic_name: str,
        message_type: type[Message],
        callback: MessageCallback | None = None,
        node_name: str = "anonymous",
        discovery_address: str = DEFAULT_DISCOVERY_ADDRESS,
        queue_size: int = 10,
        wait_for_topic: bool = True,
        topic_timeout: float = 600.0,
        context: zmq.asyncio.Context | None = None,
    ):
        """
        Initialize the subscriber.

        Args:
            topic_name: Name of the topic to subscribe to
            message_type: Type of message expected
            callback: Async callback function for received messages
            node_name: Name of the node creating this subscriber
            discovery_address: Address of the discovery daemon
            queue_size: High-water mark for incoming messages
            wait_for_topic: Whether to wait for topic to be available
            topic_timeout: Timeout for waiting for topic (seconds)
            context: Shared ZMQ async context from Node
        """
        self.topic_name = topic_name
        self.message_type = message_type
        self._callback = callback
        self.node_name = node_name
        self.discovery_address = discovery_address
        self.queue_size = queue_size
        self.topic_timeout = topic_timeout
        self._wait_for_topic = wait_for_topic

        # Connection info
        self._topic_info: TopicInfo | None = None
        self._connected = False

        # ZMQ setup - context provided by Node
        self._context: zmq.asyncio.Context = context or zmq.asyncio.Context()
        self._socket: zmq.asyncio.Socket | None = None

        # Discovery client
        self._discovery_client: DiscoveryClient | None = DiscoveryClient(
            discovery_address=self.discovery_address
        )

        # Statistics
        self._receive_count = 0
        self._last_receive_time: float | None = None

        # Executor for receive loop
        self._executor: AsyncExecutor | None = None

        # Try non-blocking connect (will succeed if topic already exists)
        self._connect()

    def _connect(self) -> bool:
        """
        Connect to the topic (non-blocking lookup only).

        Returns:
            True if connected successfully
        """
        try:
            # Non-blocking lookup only
            self._topic_info = self._discovery_client.lookup_topic(self.topic_name)
            return self._finalize_connection()

        except Exception as e:
            logger.error(f"Failed to connect to topic: {e}")
            return False

    async def _async_connect(self) -> bool:
        """
        Async connect to the topic, waiting if necessary.

        Uses DiscoveryClient.wait_for_topic_async for non-blocking wait.

        Returns:
            True if connected successfully
        """
        if self._connected:
            return True

        try:
            if self._wait_for_topic:
                logger.info(f"Waiting for topic {self.topic_name}...")
                self._topic_info = await self._discovery_client.wait_for_topic_async(
                    self.topic_name, timeout=self.topic_timeout
                )
            else:
                self._topic_info = self._discovery_client.lookup_topic(self.topic_name)

            return self._finalize_connection()

        except Exception as e:
            logger.error(f"Failed to connect to topic: {e}")
            return False

    def _finalize_connection(self) -> bool:
        """
        Finalize connection after topic info is obtained.

        Returns:
            True if connected successfully
        """
        if self._topic_info:
            # Verify message type
            if self._topic_info.fingerprint != self.message_type.fingerprint():
                logger.warning(
                    f"Message type mismatch for {self.topic_name}: "
                    f"expected {self.message_type.__name__}, "
                    f"got {self._topic_info.message_type}"
                )

            # Connect to the publisher
            self._setup_socket(self._topic_info.address)
            self._connected = True
            logger.info(
                f"Connected to topic {self.topic_name} at {self._topic_info.address}"
            )
            return True
        else:
            logger.warning(
                f"Topic {self.topic_name} not found yet, will retry in run()"
            )
            return False

    def _setup_socket(self, address: str) -> None:
        """Set up the ZMQ subscriber socket."""
        self._socket = self._context.socket(zmq.SUB)

        # Set high-water mark
        self._socket.setsockopt(zmq.RCVHWM, self.queue_size)

        # Set linger to 0 for immediate shutdown
        self._socket.setsockopt(zmq.LINGER, 0)

        # Subscribe to topic
        self._socket.setsockopt_string(zmq.SUBSCRIBE, self.topic_name)

        # Connect to publisher
        self._socket.connect(address)

        logger.debug(f"Subscriber socket connected to {address}")

    async def receive(self) -> tuple[Message, MessageHeader] | None:
        """
        Receive a single message (async).

        Returns:
            Tuple of (message, header) or None if not connected
        """
        if not self._connected or self._socket is None:
            return None

        try:
            # Receive multipart message [topic, header, metadata, *buffers]
            frames = await self._socket.recv_multipart(copy=False)

            if len(frames) < 2:
                logger.warning(f"Unexpected frame count: {len(frames)}")
                return None

            payload_frames = frames[1:]
            if len(payload_frames) == 1:
                raw_payload = (
                    memoryview(payload_frames[0].buffer)
                    if hasattr(payload_frames[0], "buffer")
                    else payload_frames[0]
                )
                message, header = self.message_type.from_bytes(raw_payload)
            else:
                message, header = self.message_type.from_frames(payload_frames)

            self._receive_count += 1
            self._last_receive_time = time.time()

            return message, header

        except asyncio.CancelledError:
            raise
        except Exception as e:
            logger.error(f"Failed to receive message: {e}")
            return None

    async def _receive_and_callback(self) -> Any:
        """Receive a message and invoke the callback."""
        result = await self.receive()
        if result:
            message, header = result
            return await self._callback(message, header)

    def start(self) -> None:
        """Start the subscriber receive loop."""
        if self._executor:
            self._executor.start()

    def stop(self) -> None:
        """Stop the subscriber receive loop."""
        if self._executor:
            self._executor.stop()

    @property
    def running(self) -> bool:
        """Check if the subscriber is running."""
        return self._executor.running if self._executor else False

    async def run(self) -> None:
        """
        Run the subscriber's async receive loop.

        Continuously receives messages and calls the callback.
        Uses AsyncExecutor for consistent execution pattern.
        """
        if self._callback is None:
            logger.warning(f"No callback set for subscriber {self.topic_name}")
            return

        if not self._connected and not await self._async_connect():
            logger.error(f"Failed to connect subscriber for {self.topic_name}")
            return

        logger.info(f"Subscriber for {self.topic_name} running")

        self._executor = AsyncExecutor(self._receive_and_callback)
        await self._executor.run()

        logger.info(f"Subscriber for {self.topic_name} stopped")

    @property
    def is_connected(self) -> bool:
        """Check if subscriber is connected to a publisher."""
        return self._connected

    @property
    def topic_info(self) -> TopicInfo | None:
        """Get information about the connected topic."""
        return self._topic_info

    @property
    def receive_count(self) -> int:
        """Get the number of messages received."""
        return self._receive_count

    @property
    def last_receive_time(self) -> float | None:
        """Get the timestamp of the last received message."""
        return self._last_receive_time

    def close(self) -> None:
        """Close the subscriber and release resources."""
        logger.info(f"Closing subscriber for {self.topic_name}")

        # Stop the executor
        if self._executor:
            self._executor.stop()
            self._executor = None

        # Close discovery client (best effort - daemon may be gone)
        if self._discovery_client:
            with contextlib.suppress(Exception):
                self._discovery_client.close()
            self._discovery_client = None

        # Close socket
        if self._socket:
            with contextlib.suppress(Exception):
                self._socket.close()
            self._socket = None

        self._connected = False
