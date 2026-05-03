"""
Publisher implementation for Cortex.

Provides a ZeroMQ-based publisher that registers with the discovery daemon
and publishes messages on IPC sockets using asyncio.
"""

import contextlib
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


def generate_ipc_address(topic_name: str, node_name: str) -> str:
    """Build the deterministic IPC endpoint for a ``(node, topic)`` pair.

    The path lives under ``/tmp/cortex/topics/`` and encodes the node name
    and topic so that the same pair always produces the same socket file.

    Args:
        topic_name: Topic path, e.g. ``/camera/image``. Leading slashes are
            converted to underscores in the filename.
        node_name: Owning node's name. Must be unique per topic within the
            host — duplicate pairs would race on the socket file.

    Returns:
        A ``ipc://...`` URI suitable for :func:`zmq.Socket.bind`.
    """
    # Create a safe filename from topic name and node name
    safe_name = node_name + "__" + topic_name.replace("/", "_").lstrip("_")

    # Ensure the directory exists
    ipc_dir = "/tmp/cortex/topics"
    os.makedirs(ipc_dir, exist_ok=True)

    return f"ipc://{ipc_dir}/{safe_name}.sock"


class Publisher:
    """Sends typed messages on a topic over a ZMQ PUB socket.

    On construction the publisher binds its own IPC socket, registers the
    ``(topic, address, fingerprint)`` triple with the discovery daemon, and
    becomes ready. :meth:`publish` is synchronous and non-blocking by default
    — if the send queue is full the message is dropped and ``False`` is
    returned.

    Always create via :meth:`Node.create_publisher`; that path shares the
    node's async context and tracks the publisher for clean shutdown.

    Note:
        ``zmq.PUB`` sockets are **not thread-safe**. Do not call
        :meth:`publish` concurrently from multiple threads or tasks on the
        same :class:`Publisher` instance.

    Example:
        ```python
        async with Node("camera_node") as node:
            pub = node.create_publisher("/camera/image", ImageMessage)
            pub.publish(ImageMessage(data=image_array))
            await node.run()
        ```
    """

    def __init__(
        self,
        topic_name: str,
        message_type: type[Message],
        node_name: str = "anonymous",
        discovery_address: str = DEFAULT_DISCOVERY_ADDRESS,
        queue_size: int = 10,
        auto_register: bool = True,
        context: zmq.asyncio.Context | zmq.Context | None = None,
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
            context: Shared ZMQ async context or sync context (optional)
        """
        self.topic_name = topic_name
        self.message_type = message_type
        self.node_name = node_name
        self.discovery_address = discovery_address
        self.queue_size = queue_size

        # Generate IPC address for this topic
        self.address = generate_ipc_address(topic_name, node_name)
        self._topic_bytes = topic_name.encode("utf-8")

        # ZMQ setup - context provided by Node
        # if context is async context, convert to sync context
        self._context: zmq.asyncio.Context | zmq.Context = context or zmq.Context()
        if isinstance(self._context, zmq.asyncio.Context):
            self._context: zmq.Context = zmq.Context(
                self._context
            )  # publishers are sync
        self._socket: zmq.Socket | None = None

        # Discovery client
        self._discovery_client: DiscoveryClient | None = None
        self._registered = False

        # Statistics
        self._publish_count = 0
        self._last_publish_time: float | None = None

        # Per-publisher monotonic sequence counter. Subscribers infer drops
        # by tracking gaps in this number per ``(publisher_node, fingerprint)``
        # pair, so it must be one-counter-per-publisher rather than the
        # class-level counter that used to live on ``Message``.
        self._sequence: int = 0

        # Initialize
        self._setup_socket()
        if auto_register:
            self._register_with_discovery()

    def _setup_socket(self) -> None:
        """Set up the ZMQ publisher socket."""
        # Ensure the IPC directory exists and remove stale socket file
        path = self.address[6:]  # Remove "ipc://" prefix
        dir_path = os.path.dirname(path)
        os.makedirs(dir_path, exist_ok=True)
        if os.path.exists(path):
            os.remove(path)

        self._socket = self._context.socket(zmq.PUB)

        # Set high-water mark (queue size)
        self._socket.setsockopt(zmq.SNDHWM, self.queue_size)

        # Set linger to 0 for immediate shutdown (close all sockets before context.term)
        self._socket.setsockopt(zmq.LINGER, 0)

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
        """Serialize and send ``message`` on this topic.

        Uses the frame-aware transport path so large NumPy / PyTorch buffers
        ride as separate ZMQ frames (zero-copy handoff).

        Args:
            message: Instance whose class matches :attr:`message_type`.
            flags: ZMQ send flags. Default :data:`zmq.NOBLOCK` — drop on
                high-water-mark rather than block the caller.

        Returns:
            ``True`` if ZMQ accepted the message; ``False`` if the queue was
            full (``zmq.Again``) or another send error was logged.

        Raises:
            TypeError: If ``type(message)`` does not match :attr:`message_type`.
        """
        if not isinstance(message, self.message_type):
            raise TypeError(
                f"Expected {self.message_type.__name__}, got {type(message).__name__}"
            )

        try:
            # Send with topic name as first frame for filtering.
            # Message payload uses frame-aware transport to keep large buffers
            # out of the metadata blob. Sequence numbers come from this
            # publisher (not the class-level fallback) so receivers can
            # detect drops per-source.
            sequence = self._sequence
            self._sequence += 1
            self._socket.send_multipart(
                [self._topic_bytes, *message.to_frames(sequence=sequence)],
                flags=flags,
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

        # Clean up IPC socket file
        assert self.address.startswith("ipc://"), (
            "CRITICAL: ADDRESS ALWAYS STARTS WITH ipc:// -- UNLESS MANUALLY CHANGED"
        )
        path = self.address[6:]  # Remove "ipc://" prefix
        if os.path.exists(path):
            with contextlib.suppress(Exception):
                os.remove(path)
