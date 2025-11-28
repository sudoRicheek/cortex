"""
Subscriber implementation for Cortex.

Provides a ZeroMQ-based subscriber that queries the discovery daemon
and subscribes to topics using IPC sockets with asyncio.
"""

from __future__ import annotations

import asyncio
import contextlib
import logging
import time
from collections.abc import Coroutine
from typing import Any, Callable

import zmq
import zmq.asyncio

from cortex.discovery.client import DiscoveryClient
from cortex.discovery.daemon import DEFAULT_DISCOVERY_ADDRESS
from cortex.discovery.protocol import TopicInfo
from cortex.messages.base import Message, MessageHeader

logger = logging.getLogger("cortex.subscriber")


# Type for async message callback
MessageCallback = Callable[[Message, MessageHeader], Coroutine[Any, Any, None]]


class Subscriber:
    """
    Subscriber for receiving messages on a topic.

    Uses ZeroMQ SUB socket over IPC for efficient local communication.
    Automatically discovers the topic using the discovery daemon.

    Example:
        async def callback(msg, header):
            print(f"Received image at {header.timestamp_ns}")

        sub = Subscriber(
            topic_name="/camera/image",
            message_type=ImageMessage,
            callback=callback
        )

        # Run as part of node
        async with Node("my_node") as node:
            node.create_subscriber("/topic", MyMsg, callback)
            await node.run()
    """

    def __init__(
        self,
        topic_name: str,
        message_type: type[Message],
        callback: MessageCallback | None = None,
        node_name: str = "anonymous",
        discovery_address: str = DEFAULT_DISCOVERY_ADDRESS,
        queue_size: int = 10,
        auto_connect: bool = True,
        wait_for_topic: bool = True,
        topic_timeout: float = 30.0,
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
            auto_connect: Whether to automatically connect on creation
            wait_for_topic: Whether to wait for topic to be available
            topic_timeout: Timeout for waiting for topic (seconds)
            context: Optional shared ZMQ async context
        """
        self.topic_name = topic_name
        self.message_type = message_type
        self._callback = callback
        self.node_name = node_name
        self.discovery_address = discovery_address
        self.queue_size = queue_size
        self.topic_timeout = topic_timeout

        # Connection info
        self._topic_info: TopicInfo | None = None
        self._connected = False

        # ZMQ setup - use provided context or create new one
        self._context: zmq.asyncio.Context = context or zmq.asyncio.Context()
        self._owns_context = context is None
        self._socket: zmq.asyncio.Socket | None = None

        # Discovery client
        self._discovery_client: DiscoveryClient | None = None

        # Statistics
        self._receive_count = 0
        self._last_receive_time: float | None = None

        # State
        self._running = False

        # Initialize
        if auto_connect:
            self._connect(wait=wait_for_topic)

    def _connect(self, wait: bool = True) -> bool:
        """
        Connect to the topic.

        Args:
            wait: Whether to wait for the topic to be available

        Returns:
            True if connected successfully
        """
        try:
            self._discovery_client = DiscoveryClient(
                discovery_address=self.discovery_address
            )

            # Look up the topic
            if wait:
                logger.info(f"Waiting for topic {self.topic_name}...")
                self._topic_info = self._discovery_client.wait_for_topic(
                    self.topic_name, timeout=self.topic_timeout
                )
            else:
                self._topic_info = self._discovery_client.lookup_topic(self.topic_name)

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
                logger.warning(f"Topic {self.topic_name} not found")
                return False

        except Exception as e:
            logger.error(f"Failed to connect to topic: {e}")
            return False

    def _setup_socket(self, address: str) -> None:
        """Set up the ZMQ subscriber socket."""
        self._socket = self._context.socket(zmq.SUB)

        # Set high-water mark
        self._socket.setsockopt(zmq.RCVHWM, self.queue_size)

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
            # Receive multipart message [topic, data]
            frames = await self._socket.recv_multipart()

            if len(frames) != 2:
                logger.warning(f"Unexpected frame count: {len(frames)}")
                return None

            # Parse the message
            _topic_bytes, data = frames
            message, header = self.message_type.from_bytes(data)

            self._receive_count += 1
            self._last_receive_time = time.time()

            return message, header

        except zmq.Again:
            return None
        except asyncio.CancelledError:
            raise
        except Exception as e:
            logger.error(f"Failed to receive message: {e}")
            return None

    def start(self) -> None:
        """Start the subscriber receive loop."""
        self._running = True

    def stop(self) -> None:
        """Stop the subscriber receive loop."""
        self._running = False

    async def run(self) -> None:
        """
        Run the subscriber's async receive loop.

        Continuously receives messages and calls the callback.
        """
        if self._callback is None:
            logger.warning(f"No callback set for subscriber {self.topic_name}")
            return

        self._running = True
        logger.info(f"Subscriber for {self.topic_name} running")

        while self._running:
            try:
                result = await self.receive()

                if result:
                    message, header = result
                    try:
                        await self._callback(message, header)
                    except Exception as e:
                        logger.error(f"Error in callback: {e}")

                await asyncio.sleep(0)  # Yield to event loop

            except asyncio.CancelledError:
                break
            except Exception as e:
                logger.error(f"Error in receive loop: {e}")
                await asyncio.sleep(0.001)

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
        """Close the subscriber."""
        logger.info(f"Closing subscriber for {self.topic_name}")

        self._running = False

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

        # Only terminate context if we own it
        if self._owns_context and self._context:
            with contextlib.suppress(Exception):
                self._context.term()
            self._context = None

        self._connected = False

    def __enter__(self) -> Subscriber:
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self.close()
