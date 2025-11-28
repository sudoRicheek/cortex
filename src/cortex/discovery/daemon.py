"""
Discovery daemon for Cortex.

The discovery daemon runs as a separate process and maintains a registry
of all active topics. Publishers register their topics, and subscribers
query for topic addresses.

Default IPC address: ipc:///tmp/cortex_discovery
"""

import contextlib
import logging
import os
import signal
import sys
import threading
import time

import zmq

from cortex.discovery.protocol import (
    DiscoveryCommand,
    DiscoveryRequest,
    DiscoveryResponse,
    DiscoveryStatus,
    TopicInfo,
)

# Configure logging
logging.basicConfig(
    level=logging.INFO, format="%(asctime)s [%(levelname)s] %(name)s: %(message)s"
)
logger = logging.getLogger("cortex.discovery")


# Default discovery address
DEFAULT_DISCOVERY_ADDRESS = "ipc:///tmp/cortex_discovery"


class DiscoveryDaemon:
    """
    Discovery daemon that maintains topic registry.

    Uses ZMQ REP socket to handle requests from publishers and subscribers.
    """

    def __init__(
        self,
        address: str = DEFAULT_DISCOVERY_ADDRESS,
        cleanup_interval: float = 30.0,
    ):
        """
        Initialize the discovery daemon.

        Args:
            address: ZMQ address to bind to (default: ipc:///tmp/cortex_discovery)
            cleanup_interval: Interval in seconds for cleaning up stale topics
        """
        self.address = address
        self.cleanup_interval = cleanup_interval

        # Topic registry: topic_name -> TopicInfo
        self._topics: dict[str, TopicInfo] = {}
        self._topics_lock = threading.Lock()

        # Last heartbeat time for each topic
        self._heartbeats: dict[str, float] = {}

        # ZMQ context and socket
        self._context: zmq.Context | None = None
        self._socket: zmq.Socket | None = None

        # Control flags
        self._running = False
        self._shutdown_event = threading.Event()

    def _ensure_ipc_path(self) -> None:
        """Ensure the IPC socket directory exists."""
        if self.address.startswith("ipc://"):
            path = self.address[6:]  # Remove "ipc://"
            dir_path = os.path.dirname(path)
            if dir_path and not os.path.exists(dir_path):
                os.makedirs(dir_path, exist_ok=True)
            # Remove stale socket file if it exists
            if os.path.exists(path):
                os.remove(path)

    def start(self) -> None:
        """Start the discovery daemon."""
        logger.info(f"Starting discovery daemon at {self.address}")

        self._ensure_ipc_path()

        self._context = zmq.Context()
        self._socket = self._context.socket(zmq.REP)
        self._socket.bind(self.address)

        # Set socket options for responsiveness
        self._socket.setsockopt(zmq.RCVTIMEO, 1000)  # 1 second timeout

        self._running = True
        self._shutdown_event.clear()

        logger.info("Discovery daemon started")

        try:
            self._run_loop()
        except KeyboardInterrupt:
            logger.info("Received interrupt signal")
        finally:
            self.stop()

    def _run_loop(self) -> None:
        """Main event loop."""
        last_cleanup = time.time()

        while self._running and not self._shutdown_event.is_set():
            try:
                # Try to receive a request
                try:
                    request_bytes = self._socket.recv(zmq.NOBLOCK)
                except zmq.Again:
                    # No message available, continue
                    time.sleep(0.01)

                    # Periodic cleanup
                    if time.time() - last_cleanup > self.cleanup_interval:
                        self._cleanup_stale_topics()
                        last_cleanup = time.time()
                    continue

                # Process the request
                response = self._handle_request(request_bytes)
                self._socket.send(response.to_bytes())

            except Exception as e:
                logger.error(f"Error in discovery loop: {e}")
                # Send error response if we received a request
                try:
                    error_response = DiscoveryResponse(
                        status=DiscoveryStatus.ERROR, message=str(e)
                    )
                    self._socket.send(error_response.to_bytes())
                except Exception as send_err:
                    logger.debug(f"Failed to send error response: {send_err}")

    def _handle_request(self, request_bytes: bytes) -> DiscoveryResponse:
        """Handle a discovery request."""
        try:
            request = DiscoveryRequest.from_bytes(request_bytes)
        except Exception as e:
            return DiscoveryResponse(
                status=DiscoveryStatus.ERROR, message=f"Failed to parse request: {e}"
            )

        if request.command == DiscoveryCommand.REGISTER_TOPIC:
            return self._handle_register(request)
        elif request.command == DiscoveryCommand.UNREGISTER_TOPIC:
            return self._handle_unregister(request)
        elif request.command == DiscoveryCommand.LOOKUP_TOPIC:
            return self._handle_lookup(request)
        elif request.command == DiscoveryCommand.LIST_TOPICS:
            return self._handle_list()
        elif request.command == DiscoveryCommand.HEARTBEAT:
            return self._handle_heartbeat(request)
        elif request.command == DiscoveryCommand.SHUTDOWN:
            return self._handle_shutdown()
        else:
            return DiscoveryResponse(
                status=DiscoveryStatus.ERROR,
                message=f"Unknown command: {request.command}",
            )

    def _handle_register(self, request: DiscoveryRequest) -> DiscoveryResponse:
        """Handle topic registration."""
        if not request.topic_info:
            return DiscoveryResponse(
                status=DiscoveryStatus.ERROR,
                message="Missing topic_info in register request",
            )

        topic_name = request.topic_info.name

        with self._topics_lock:
            if topic_name in self._topics:
                # Allow re-registration from same publisher
                existing = self._topics[topic_name]
                if existing.publisher_node != request.topic_info.publisher_node:
                    return DiscoveryResponse(
                        status=DiscoveryStatus.ALREADY_EXISTS,
                        message=f"Topic {topic_name} already registered by {existing.publisher_node}",
                    )

            self._topics[topic_name] = request.topic_info
            self._heartbeats[topic_name] = time.time()

        logger.info(f"Registered topic: {topic_name} at {request.topic_info.address}")

        return DiscoveryResponse(
            status=DiscoveryStatus.OK, message=f"Registered topic: {topic_name}"
        )

    def _handle_unregister(self, request: DiscoveryRequest) -> DiscoveryResponse:
        """Handle topic unregistration."""
        topic_name = request.topic_name or (
            request.topic_info.name if request.topic_info else None
        )

        if not topic_name:
            return DiscoveryResponse(
                status=DiscoveryStatus.ERROR,
                message="Missing topic name in unregister request",
            )

        with self._topics_lock:
            if topic_name not in self._topics:
                return DiscoveryResponse(
                    status=DiscoveryStatus.NOT_FOUND,
                    message=f"Topic {topic_name} not found",
                )

            del self._topics[topic_name]
            self._heartbeats.pop(topic_name, None)

        logger.info(f"Unregistered topic: {topic_name}")

        return DiscoveryResponse(
            status=DiscoveryStatus.OK, message=f"Unregistered topic: {topic_name}"
        )

    def _handle_lookup(self, request: DiscoveryRequest) -> DiscoveryResponse:
        """Handle topic lookup."""
        topic_name = request.topic_name

        if not topic_name:
            return DiscoveryResponse(
                status=DiscoveryStatus.ERROR,
                message="Missing topic_name in lookup request",
            )

        with self._topics_lock:
            topic_info = self._topics.get(topic_name)

        if topic_info:
            return DiscoveryResponse(status=DiscoveryStatus.OK, topic_info=topic_info)
        else:
            return DiscoveryResponse(
                status=DiscoveryStatus.NOT_FOUND,
                message=f"Topic {topic_name} not found",
            )

    def _handle_list(self) -> DiscoveryResponse:
        """Handle list all topics."""
        with self._topics_lock:
            topics = list(self._topics.values())

        return DiscoveryResponse(status=DiscoveryStatus.OK, topics=topics)

    def _handle_heartbeat(self, request: DiscoveryRequest) -> DiscoveryResponse:
        """Handle heartbeat from publisher."""
        topic_name = request.topic_name

        if not topic_name:
            return DiscoveryResponse(
                status=DiscoveryStatus.ERROR,
                message="Missing topic_name in heartbeat request",
            )

        with self._topics_lock:
            if topic_name in self._topics:
                self._heartbeats[topic_name] = time.time()
                return DiscoveryResponse(status=DiscoveryStatus.OK)
            else:
                return DiscoveryResponse(
                    status=DiscoveryStatus.NOT_FOUND,
                    message=f"Topic {topic_name} not registered",
                )

    def _handle_shutdown(self) -> DiscoveryResponse:
        """Handle shutdown request."""
        logger.info("Received shutdown command")
        self._running = False
        self._shutdown_event.set()
        return DiscoveryResponse(status=DiscoveryStatus.OK, message="Shutting down")

    def _cleanup_stale_topics(self) -> None:
        """Remove topics that haven't sent a heartbeat recently."""
        stale_threshold = time.time() - (self.cleanup_interval * 3)

        with self._topics_lock:
            stale_topics = [
                name
                for name, last_beat in self._heartbeats.items()
                if last_beat < stale_threshold
            ]

            for topic_name in stale_topics:
                logger.warning(f"Removing stale topic: {topic_name}")
                del self._topics[topic_name]
                del self._heartbeats[topic_name]

    def stop(self) -> None:
        """Stop the discovery daemon."""
        logger.info("Stopping discovery daemon")
        self._running = False
        self._shutdown_event.set()

        if self._socket:
            try:
                self._socket.setsockopt(zmq.LINGER, 0)
                self._socket.close()
            except Exception as e:
                logger.debug(f"Error closing socket: {e}")
            self._socket = None

        if self._context:
            try:
                self._context.term()
            except zmq.ZMQError as e:
                logger.debug(f"Error terminating context: {e}")
            self._context = None

        # Clean up IPC socket file
        if self.address.startswith("ipc://"):
            path = self.address[6:]
            if os.path.exists(path):
                with contextlib.suppress(Exception):
                    os.remove(path)

        logger.info("Discovery daemon stopped")


def main():
    """Entry point for the discovery daemon."""
    import argparse

    parser = argparse.ArgumentParser(description="Cortex Discovery Daemon")
    parser.add_argument(
        "--address",
        default=DEFAULT_DISCOVERY_ADDRESS,
        help=f"ZMQ address to bind to (default: {DEFAULT_DISCOVERY_ADDRESS})",
    )
    parser.add_argument(
        "--cleanup-interval",
        type=float,
        default=30.0,
        help="Interval for cleaning up stale topics (default: 30s)",
    )
    parser.add_argument(
        "--log-level",
        default="INFO",
        choices=["DEBUG", "INFO", "WARNING", "ERROR"],
        help="Logging level (default: INFO)",
    )

    args = parser.parse_args()

    # Set log level
    logging.getLogger().setLevel(getattr(logging, args.log_level))

    # Create and run daemon
    daemon = DiscoveryDaemon(
        address=args.address, cleanup_interval=args.cleanup_interval
    )

    # Handle signals
    def signal_handler(signum, frame):
        logger.info(f"Received signal {signum}")
        daemon.stop()
        sys.exit(0)

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    daemon.start()


if __name__ == "__main__":
    main()
