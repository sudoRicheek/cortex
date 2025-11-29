"""
Discovery client for Cortex.

Provides a client interface to interact with the discovery daemon.
Uses synchronous ZMQ since discovery is typically done once at startup.
"""

import asyncio
import contextlib
import logging
import threading
import time

import zmq

from cortex.discovery.daemon import DEFAULT_DISCOVERY_ADDRESS
from cortex.discovery.protocol import (
    DiscoveryCommand,
    DiscoveryRequest,
    DiscoveryResponse,
    DiscoveryStatus,
    TopicInfo,
)

logger = logging.getLogger("cortex.discovery.client")


class DiscoveryClient:
    """
    Client for interacting with the discovery daemon.

    Provides methods for registering, unregistering, and looking up topics.
    Uses synchronous ZMQ since discovery is typically done at startup.
    """

    def __init__(
        self,
        discovery_address: str = DEFAULT_DISCOVERY_ADDRESS,
        timeout_ms: int = 5000,
        retries: int = 1,
    ):
        """
        Initialize the discovery client.

        Args:
            discovery_address: Address of the discovery daemon
            timeout_ms: Request timeout in milliseconds
            retries: Number of retries for failed requests
        """
        self.discovery_address = discovery_address
        self.timeout_ms = timeout_ms
        self.retries = retries

        self._context: zmq.Context = zmq.Context()
        self._socket: zmq.Socket | None = None
        self._lock = threading.Lock()

        # Heartbeat thread for registered topics
        self._heartbeat_topics: dict[str, bool] = {}
        self._heartbeat_thread: threading.Thread | None = None
        self._heartbeat_running = False

        # Connect immediately
        self._connect()

    def _connect(self) -> None:
        """Create and connect the socket."""
        self._socket = self._context.socket(zmq.REQ)
        self._socket.setsockopt(zmq.RCVTIMEO, self.timeout_ms)
        self._socket.setsockopt(zmq.SNDTIMEO, self.timeout_ms)
        self._socket.setsockopt(zmq.LINGER, 0)  # Immediate shutdown
        self._socket.connect(self.discovery_address)

    def _reconnect(self) -> None:
        """Reconnect by closing and recreating the socket.

        This is needed because REQ sockets get stuck in a bad state
        after a timeout (waiting for reply that will never come).
        """
        if self._socket:
            with contextlib.suppress(Exception):
                self._socket.close()
        self._connect()

    def _send_request(self, request: DiscoveryRequest) -> DiscoveryResponse:
        """Send a request and wait for response."""

        last_error: Exception | None = None

        for attempt in range(self.retries):
            try:
                with self._lock:
                    self._socket.send(request.to_bytes())
                    response_bytes = self._socket.recv()
                    return DiscoveryResponse.from_bytes(response_bytes)
            except zmq.Again:
                # Timeout - need to reconnect because REQ socket is now stuck
                last_error = TimeoutError(
                    f"Discovery request timed out after {self.timeout_ms}ms"
                )
                logger.warning(f"Request timeout, attempt {attempt + 1}/{self.retries}")
                self._reconnect()
            except zmq.ZMQError as e:
                # ZMQ error - reconnect and re-raise
                last_error = e
                logger.warning(f"ZMQ error: {e}, attempt {attempt + 1}/{self.retries}")
                self._reconnect()

        raise last_error

    def register_topic(self, topic_info: TopicInfo) -> bool:
        """
        Register a topic with the discovery daemon.

        Args:
            topic_info: Information about the topic to register

        Returns:
            True if registration was successful
        """
        request = DiscoveryRequest(
            command=DiscoveryCommand.REGISTER_TOPIC, topic_info=topic_info
        )

        try:
            response = self._send_request(request)
            if response.status == DiscoveryStatus.OK:
                # Start heartbeat for this topic
                self._start_heartbeat(topic_info.name)
                logger.info(f"Registered topic: {topic_info.name}")
                return True
            else:
                logger.error(f"Failed to register topic: {response.message}")
                return False
        except Exception as e:
            logger.error(f"Failed to register topic: {e}")
            return False

    def unregister_topic(self, topic_name: str) -> bool:
        """
        Unregister a topic from the discovery daemon.

        Args:
            topic_name: Name of the topic to unregister

        Returns:
            True if unregistration was successful
        """
        # Stop heartbeat for this topic
        self._stop_heartbeat(topic_name)

        request = DiscoveryRequest(
            command=DiscoveryCommand.UNREGISTER_TOPIC, topic_name=topic_name
        )

        try:
            response = self._send_request(request)
            if response.status == DiscoveryStatus.OK:
                logger.info(f"Unregistered topic: {topic_name}")
                return True
            else:
                logger.warning(f"Failed to unregister topic: {response.message}")
                return False
        except Exception as e:
            logger.error(f"Failed to unregister topic: {e}")
            return False

    def lookup_topic(self, topic_name: str) -> TopicInfo | None:
        """
        Look up a topic by name.

        Args:
            topic_name: Name of the topic to look up

        Returns:
            TopicInfo if found, None otherwise
        """
        request = DiscoveryRequest(
            command=DiscoveryCommand.LOOKUP_TOPIC, topic_name=topic_name
        )

        try:
            response = self._send_request(request)
            if response.status == DiscoveryStatus.OK:
                return response.topic_info
            else:
                return None
        except TimeoutError:
            logger.error(
                f"Lookup timeout for topic: {topic_name}. Probably Discovery Daemon is not running."
            )
            return None
        except Exception as e:
            logger.error(f"Failed to lookup topic: {e}")
            return None

    def wait_for_topic(
        self,
        topic_name: str,
        timeout: float = 30.0,
        poll_interval: float = 0.5,
    ) -> TopicInfo | None:
        """
        Wait for a topic to become available (blocking).

        Args:
            topic_name: Name of the topic to wait for
            timeout: Maximum time to wait in seconds
            poll_interval: Time between lookup attempts in seconds

        Returns:
            TopicInfo if found within timeout, None otherwise
        """
        start_time = time.time()

        while time.time() - start_time < timeout:
            topic_info = self.lookup_topic(topic_name)
            if topic_info:
                return topic_info
            time.sleep(poll_interval)

        return None

    async def wait_for_topic_async(
        self,
        topic_name: str,
        timeout: float = 600.0,
        poll_interval: float = 0.5,
    ) -> TopicInfo | None:
        """
        Wait for a topic to become available (async, non-blocking).

        Uses asyncio.sleep to avoid blocking the event loop.

        Args:
            topic_name: Name of the topic to wait for
            timeout: Maximum time to wait in seconds
            poll_interval: Time between lookup attempts in seconds

        Returns:
            TopicInfo if found within timeout, None otherwise
        """
        start_time = time.perf_counter()

        while time.perf_counter() - start_time < timeout:
            topic_info = self.lookup_topic(topic_name)
            if topic_info:
                return topic_info
            await asyncio.sleep(poll_interval)

        return None

    def list_topics(self) -> list[TopicInfo]:
        """
        List all registered topics.

        Returns:
            List of TopicInfo for all registered topics
        """
        request = DiscoveryRequest(command=DiscoveryCommand.LIST_TOPICS)

        try:
            response = self._send_request(request)
            if response.status == DiscoveryStatus.OK:
                return response.topics or []
            else:
                logger.warning(f"Failed to list topics: {response.message}")
                return []
        except Exception as e:
            logger.error(f"Failed to list topics: {e}")
            return []

    def _start_heartbeat(self, topic_name: str) -> None:
        """Start sending heartbeats for a topic."""
        self._heartbeat_topics[topic_name] = True

        if not self._heartbeat_running:
            self._heartbeat_running = True
            self._heartbeat_thread = threading.Thread(
                target=self._heartbeat_loop, daemon=True
            )
            self._heartbeat_thread.start()

    def _stop_heartbeat(self, topic_name: str) -> None:
        """Stop sending heartbeats for a topic."""
        self._heartbeat_topics.pop(topic_name, None)

    def _heartbeat_loop(self) -> None:
        """Background thread that sends heartbeats for registered topics."""
        while self._heartbeat_running and self._heartbeat_topics:
            for topic_name in list(self._heartbeat_topics.keys()):
                if not self._heartbeat_running:
                    break
                if topic_name in self._heartbeat_topics:
                    self._send_heartbeat(topic_name)

            time.sleep(10.0)  # Heartbeat interval

    def _send_heartbeat(self, topic_name: str) -> None:
        """Send a heartbeat for a topic."""
        request = DiscoveryRequest(
            command=DiscoveryCommand.HEARTBEAT, topic_name=topic_name
        )

        try:
            self._send_request(request)
        except Exception as e:
            logger.warning(f"Failed to send heartbeat for {topic_name}: {e}")

    def close(self) -> None:
        """Close the client connection."""
        self._heartbeat_running = False
        self._heartbeat_topics.clear()

        if self._socket:
            with contextlib.suppress(Exception):
                self._socket.close()
            self._socket = None

        with contextlib.suppress(Exception):
            self._context.term()

    def __enter__(self) -> "DiscoveryClient":
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self.close()
