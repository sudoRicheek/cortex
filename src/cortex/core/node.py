"""
Node abstraction for Cortex.

Provides an async interface for managing publishers and subscribers.
Uses asyncio for cooperative multitasking - ideal for Python < 3.14.
"""

import asyncio
import logging
from collections.abc import Callable, Coroutine
from typing import Any

import zmq
import zmq.asyncio

from cortex.core.executor import AsyncExecutor, RateExecutor
from cortex.core.publisher import Publisher
from cortex.core.subscriber import Subscriber
from cortex.discovery.daemon import DEFAULT_DISCOVERY_ADDRESS
from cortex.messages.base import Message

logger = logging.getLogger("cortex.node")


# Type aliases
AsyncCallback = Callable[..., Coroutine[Any, Any, None]]
MessageCallback = Callable[[Message, Any], Coroutine[Any, Any, None]]


class Node:
    """
    A node in the Cortex communication graph.

    Nodes manage a collection of publishers and subscribers using asyncio
    for cooperative multitasking.

    Example:
        class CameraNode(Node):
            def __init__(self):
                super().__init__("camera_node")

                self.pub = self.create_publisher(
                    "/camera/image",
                    ImageMessage
                )

                self.create_timer(1/30, self.publish_image)

            async def publish_image(self):
                image = capture_image()
                self.pub.publish(ImageMessage(data=image))

            async def run(self):
                await super().run()
    """

    def __init__(
        self,
        name: str,
        discovery_address: str = DEFAULT_DISCOVERY_ADDRESS,
    ):
        """
        Initialize the node.

        Args:
            name: Unique name for this node
            discovery_address: Address of the discovery daemon
        """
        self.name = name
        self.discovery_address = discovery_address

        # ZMQ async context
        self._context = zmq.asyncio.Context()

        # Publishers and subscribers
        self._publishers: dict[str, Publisher] = {}
        self._subscribers: dict[str, Subscriber] = {}

        # Timer executors: (period, callback, RateExecutor)
        self._timers: list[tuple[float, AsyncCallback, Any]] = []

        # Async executors (non-rate-limited)
        self._async_executors: list[Any] = []

        # Tasks
        self._tasks: list[asyncio.Task] = []

        # State
        self._running = False

        logger.info(f"Created node: {name}")

    def create_publisher(
        self,
        topic_name: str,
        message_type: type[Message],
        queue_size: int = 10,
    ) -> Publisher:
        """
        Create a publisher for a topic.

        Args:
            topic_name: Name of the topic
            message_type: Type of messages to publish
            queue_size: Output queue size

        Returns:
            Publisher instance
        """
        if topic_name in self._publishers:
            logger.warning(f"Publisher for {topic_name} already exists")
            return self._publishers[topic_name]

        pub = Publisher(
            topic_name=topic_name,
            message_type=message_type,
            node_name=self.name,
            discovery_address=self.discovery_address,
            queue_size=queue_size,
            context=self._context,
        )

        self._publishers[topic_name] = pub
        logger.info(f"Created publisher for {topic_name}")

        return pub

    def create_subscriber(
        self,
        topic_name: str,
        message_type: type[Message],
        callback: MessageCallback | None = None,
        queue_size: int = 10,
        wait_for_topic: bool = True,
        topic_timeout: float = 30.0,
    ) -> Subscriber:
        """
        Create a subscriber for a topic.

        Args:
            topic_name: Name of the topic
            message_type: Type of messages expected
            callback: Async function to call when messages are received
            queue_size: Input queue size
            wait_for_topic: Whether to wait for the topic to be available
            topic_timeout: Timeout for waiting for topic

        Returns:
            Subscriber instance
        """
        if topic_name in self._subscribers:
            logger.warning(f"Subscriber for {topic_name} already exists")
            return self._subscribers[topic_name]

        sub = Subscriber(
            topic_name=topic_name,
            message_type=message_type,
            callback=callback,
            node_name=self.name,
            discovery_address=self.discovery_address,
            queue_size=queue_size,
            wait_for_topic=wait_for_topic,
            topic_timeout=topic_timeout,
            context=self._context,
        )

        self._subscribers[topic_name] = sub
        logger.info(f"Created subscriber for {topic_name}")

        # Add subscriber's receive loop as an async executor
        if callback is not None:
            self._async_executors.append(sub)

        return sub

    def create_timer(
        self,
        period: float,
        callback: AsyncCallback,
    ) -> None:
        """
        Create a periodic timer.

        Args:
            period: Timer period in seconds
            callback: Async function to call on each timer tick
        """
        rate_hz = 1.0 / period
        executor = RateExecutor(callback, rate_hz=rate_hz)
        self._timers.append((period, callback, executor))

        logger.debug(f"Created timer with period {period}s ({rate_hz} Hz)")

    def create_async_executor(self, callback: AsyncCallback) -> None:
        """
        Create an async executor that runs as fast as possible.

        Args:
            callback: Async function to execute continuously
        """
        executor = AsyncExecutor(callback)
        self._async_executors.append(executor)

        logger.debug("Created async executor")

    async def run(self) -> None:
        """
        Run the node, processing messages and timers.

        This is the main async entry point for the node.
        """
        self._running = True

        # Start all timer executors
        for _period, _callback, executor in self._timers:
            executor.start()
            self._tasks.append(asyncio.create_task(executor.run()))

        # Start all async executors (including subscriber receive loops)
        for executor in self._async_executors:
            if hasattr(executor, "start"):
                executor.start()
            if hasattr(executor, "run"):
                self._tasks.append(asyncio.create_task(executor.run()))

        logger.info(f"Node {self.name} running with {len(self._tasks)} tasks")

        try:
            await asyncio.gather(*self._tasks, return_exceptions=True)
        except asyncio.CancelledError:
            logger.info(f"Node {self.name} cancelled")
        finally:
            self._running = False
            # Stop all executors
            for _period, _callback, executor in self._timers:
                executor.stop()
            for executor in self._async_executors:
                if hasattr(executor, "stop"):
                    executor.stop()

    def stop(self) -> None:
        """Stop the node."""
        logger.info(f"Stopping node {self.name}")
        self._running = False

        # Stop all executors
        for _period, _callback, executor in self._timers:
            executor.stop()
        for executor in self._async_executors:
            if hasattr(executor, "stop"):
                executor.stop()

        # Cancel all tasks
        for task in self._tasks:
            if not task.done():
                task.cancel()

    async def close(self) -> None:
        """Close the node and release all resources."""
        logger.info(f"Closing node {self.name}")

        self.stop()

        # Wait for tasks to complete
        if self._tasks:
            await asyncio.gather(*self._tasks, return_exceptions=True)
        self._tasks.clear()

        # Close all publishers
        for pub in self._publishers.values():
            pub.close()
        self._publishers.clear()

        # Close all subscribers
        for sub in self._subscribers.values():
            sub.close()
        self._subscribers.clear()

        self._timers.clear()
        self._async_executors.clear()

        # Terminate ZMQ context
        self._context.term()

        logger.info(f"Node {self.name} closed")

    def get_publisher(self, topic_name: str) -> Publisher | None:
        """Get a publisher by topic name."""
        return self._publishers.get(topic_name)

    def get_subscriber(self, topic_name: str) -> Subscriber | None:
        """Get a subscriber by topic name."""
        return self._subscribers.get(topic_name)

    @property
    def publishers(self) -> list[str]:
        """Get list of publisher topic names."""
        return list(self._publishers.keys())

    @property
    def subscribers(self) -> list[str]:
        """Get list of subscriber topic names."""
        return list(self._subscribers.keys())

    @property
    def is_running(self) -> bool:
        """Check if the node is running."""
        return self._running

    async def __aenter__(self) -> "Node":
        return self

    async def __aexit__(self, exc_type, exc_val, exc_tb) -> None:
        await self.close()
