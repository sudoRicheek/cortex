"""
Publisher implementation for Cortex.

Provides a ZeroMQ-based publisher that registers with the discovery daemon
and publishes messages on IPC sockets using asyncio.
"""

from __future__ import annotations

import contextlib
import hashlib
import logging
import os
import time

import zmq
import zmq.asyncio

from cortex.discovery.client import DiscoveryClient
from cortex.discovery.daemon import DEFAULT_DISCOVERY_ADDRESS
from cortex.discovery.protocol import TopicInfo
from cortex.messages.base import Message

logger = logging.getLogger("cortex.publisher")


def generate_ipc_address(topic_name: str) -> str:
    """
    Generate a unique IPC address for a topic.

    Uses a hash of the topic name to create a valid filesystem path.
    """
    # Create a safe filename from topic name
    safe_name = topic_name.replace("/", "_").lstrip("_")
    # Add hash suffix for uniqueness
    hash_suffix = hashlib.md5(topic_name.encode()).hexdigest()[:8]

    # Ensure the directory exists
    ipc_dir = "/tmp/cortex/topics"
    os.makedirs(ipc_dir, exist_ok=True)

    return f"ipc://{ipc_dir}/{safe_name}_{hash_suffix}"


class Publisher:
    """
    Publisher for sending messages on a topic.

    Uses ZeroMQ PUB socket over IPC for efficient local communication.
    Automatically registers with the discovery daemon.

    Example:
        pub = Publisher(
            topic_name="/camera/image",
            message_type=ImageMessage,
            node_name="camera_node"
        )
        pub.publish(ImageMessage(data=image_array))
    """

    def __init__(
        self,
        topic_name: str,
        message_type: type[Message],
        node_name: str = "anonymous",
        discovery_address: str = DEFAULT_DISCOVERY_ADDRESS,
        queue_size: int = 10,
        auto_register: bool = True,
        context: zmq.asyncio.Context | None = None,
    ):
        """
        Initialize the publisher.

        Args:
            topic_name: Name of the topic to publish on (e.g., "/camera/image")
            message_type: Type of message to publish
            node_name: Name of the node creating this publisher
            discovery_address: Address of the discovery daemon
            queue_size: High-water mark for outgoing messages
            auto_register: Whether to automatically register with discovery daemon
            context: Optional shared ZMQ async context
        """
        self.topic_name = topic_name
        self.message_type = message_type
        self.node_name = node_name
        self.discovery_address = discovery_address
        self.queue_size = queue_size

        # Generate IPC address for this topic
        self.address = generate_ipc_address(topic_name)

        # ZMQ setup - use provided context or create new one
        self._context: zmq.asyncio.Context = context or zmq.asyncio.Context()
        self._owns_context = context is None
        self._socket: zmq.asyncio.Socket | None = None

        # Discovery client
        self._discovery_client: DiscoveryClient | None = None
        self._registered = False

        # Statistics
        self._publish_count = 0
        self._last_publish_time: float | None = None

        # Initialize
        self._setup_socket()
        if auto_register:
            self._register_with_discovery()

    def _setup_socket(self) -> None:
        """Set up the ZMQ publisher socket."""
        # Ensure the IPC directory exists
        if self.address.startswith("ipc://"):
            path = self.address[6:]
            dir_path = os.path.dirname(path)
            os.makedirs(dir_path, exist_ok=True)
            # Remove stale socket file
            if os.path.exists(path):
                os.remove(path)

        self._socket = self._context.socket(zmq.PUB)

        # Set high-water mark (queue size)
        self._socket.setsockopt(zmq.SNDHWM, self.queue_size)

        # Bind to the address
        self._socket.bind(self.address)

        logger.debug(f"Publisher socket bound to {self.address}")

    def _register_with_discovery(self) -> None:
        """Register this publisher with the discovery daemon."""
        try:
            self._discovery_client = DiscoveryClient(
                discovery_address=self.discovery_address
            )

            topic_info = TopicInfo(
                name=self.topic_name,
                address=self.address,
                message_type=self.message_type.__name__,
                fingerprint=self.message_type.fingerprint(),
                publisher_node=self.node_name,
            )

            if self._discovery_client.register_topic(topic_info):
                self._registered = True
                logger.info(f"Registered topic {self.topic_name} with discovery daemon")
            else:
                logger.warning(f"Failed to register topic {self.topic_name}")
        except Exception as e:
            logger.warning(f"Could not connect to discovery daemon: {e}")

    def publish(self, message: Message, flags: int = zmq.NOBLOCK) -> bool:
        """
        Publish a message (non-blocking).

        Args:
            message: The message to publish (must match message_type)
            flags: ZMQ flags for sending (default: NOBLOCK)

        Returns:
            True if the message was sent successfully

        Raises:
            TypeError: If message type doesn't match
        """
        if not isinstance(message, self.message_type):
            raise TypeError(
                f"Expected {self.message_type.__name__}, got {type(message).__name__}"
            )

        try:
            # Serialize and send
            data = message.to_bytes()

            # Send with topic name as first frame for filtering
            self._socket.send_multipart(
                [self.topic_name.encode("utf-8"), data], flags=flags
            )

            self._publish_count += 1
            self._last_publish_time = time.time()

            return True
        except zmq.Again:
            # Would block - queue full
            return False
        except Exception as e:
            logger.error(f"Failed to publish message: {e}")
            return False

    @property
    def is_registered(self) -> bool:
        """Check if publisher is registered with discovery daemon."""
        return self._registered

    @property
    def publish_count(self) -> int:
        """Get the number of messages published."""
        return self._publish_count

    @property
    def last_publish_time(self) -> float | None:
        """Get the timestamp of the last published message."""
        return self._last_publish_time

    def close(self) -> None:
        """Close the publisher and unregister from discovery."""
        logger.info(f"Closing publisher for {self.topic_name}")

        # Unregister from discovery (best effort - daemon may be gone)
        if self._discovery_client and self._registered:
            with contextlib.suppress(Exception):
                self._discovery_client.unregister_topic(self.topic_name)
            with contextlib.suppress(Exception):
                self._discovery_client.close()
            self._discovery_client = None

        self._registered = False

        # Close socket
        if self._socket:
            self._socket.close()
            self._socket = None

        # Only terminate context if we own it
        if self._owns_context and self._context:
            self._context.term()
            self._context = None

        # Clean up IPC socket file
        if self.address.startswith("ipc://"):
            path = self.address[6:]
            if os.path.exists(path):
                with contextlib.suppress(Exception):
                    os.remove(path)

    def __enter__(self) -> Publisher:
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self.close()
