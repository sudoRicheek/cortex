"""
Discovery daemon for Cortex.

The discovery daemon runs as a separate process and maintains a registry
of all active topics. Publishers register their topics, and subscribers
query for topic addresses.

Default IPC address: ipc:///tmp/cortex/discovery.sock
"""

import contextlib
import os

import zmq

from cortex.discovery.protocol import (
    DiscoveryCommand,
    DiscoveryRequest,
    DiscoveryResponse,
    DiscoveryStatus,
    TopicInfo,
)
from cortex.utils.logging import get_logger, set_log_level

# Get logger for this module
logger = get_logger("cortex.discovery")


# Default discovery address
DEFAULT_DISCOVERY_ADDRESS = "ipc:///tmp/cortex/discovery.sock"


class DiscoveryDaemon:
    """
    Discovery daemon that maintains topic registry.

    Uses ZMQ REP socket to handle requests from publishers and subscribers.
    """

    def __init__(
        self,
        address: str = DEFAULT_DISCOVERY_ADDRESS,
    ):
        """
        Initialize the discovery daemon.

        Args:
            address: ZMQ address to bind to (default: ipc:///tmp/cortex/discovery.sock)
        """
        self.address = address

        # Topic registry: topic_name -> TopicInfo
        self._topics: dict[str, TopicInfo] = {}

        # ZMQ context and socket
        self._context: zmq.Context | None = None
        self._socket: zmq.Socket | None = None

        # Control flag
        self._running = False

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

        #! We do not set a high water mark on the socket.
        #! It is 1000 by default, which is reasonable for our use case.

        # Set socket options for responsiveness
        self._socket.setsockopt(zmq.RCVTIMEO, 1000)  # 1 second timeout
        self._socket.setsockopt(zmq.LINGER, 0)  # Immediate shutdown

        self._running = True

        logger.info("=" * 50)
        logger.info("DISCOVERY DAEMON STARTED")
        logger.info("  Address: %s", self.address)
        logger.info("=" * 50)

        try:
            self._run_loop()
        except KeyboardInterrupt:
            logger.info("Received interrupt signal")
        finally:
            self._cleanup()

    def _run_loop(self) -> None:
        """Main event loop."""
        while self._running:
            try:
                # Try to receive a request (blocks up to RCVTIMEO)
                try:
                    request_bytes = self._socket.recv(copy=False)

                    # Process the request
                    response = self._handle_request(request_bytes)
                    self._socket.send(response.to_bytes())
                except zmq.Again:
                    # Timeout, check _running and continue
                    continue

            except Exception as e:
                if not self._running:
                    # We're shutting down, exit cleanly
                    break
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

        if topic_name in self._topics:
            # Allow re-registration from same publisher
            existing = self._topics[topic_name]
            if existing.publisher_node != request.topic_info.publisher_node:
                return DiscoveryResponse(
                    status=DiscoveryStatus.ALREADY_EXISTS,
                    message=f"Topic {topic_name} already registered by {existing.publisher_node}",
                )

        self._topics[topic_name] = request.topic_info

        logger.info("-" * 50)
        logger.info("REGISTER topic: %s", topic_name)
        logger.info("  Address:     %s", request.topic_info.address)
        logger.info("  Publisher:   %s", request.topic_info.publisher_node)
        logger.info("  Type:        %s", request.topic_info.message_type)
        logger.info("  Fingerprint: %d", request.topic_info.fingerprint)

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

        if topic_name not in self._topics:
            return DiscoveryResponse(
                status=DiscoveryStatus.NOT_FOUND,
                message=f"Topic {topic_name} not found",
            )

        del self._topics[topic_name]

        logger.info("-" * 50)
        logger.info("UNREGISTER topic: %s", topic_name)

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

        topic_info = self._topics.get(topic_name)

        logger.info("-" * 50)
        if topic_info:
            logger.info("LOOKUP topic: %s -> FOUND", topic_name)
            return DiscoveryResponse(status=DiscoveryStatus.OK, topic_info=topic_info)
        else:
            logger.info("LOOKUP topic: %s -> NOT FOUND", topic_name)
            return DiscoveryResponse(
                status=DiscoveryStatus.NOT_FOUND,
                message=f"Topic {topic_name} not found",
            )

    def _handle_list(self) -> DiscoveryResponse:
        """Handle list all topics."""
        topics = list(self._topics.values())

        logger.info("-" * 50)
        logger.info("LIST topics: %d registered", len(topics))

        return DiscoveryResponse(status=DiscoveryStatus.OK, topics=topics)

    def _handle_shutdown(self) -> DiscoveryResponse:
        """Handle shutdown request."""
        self._running = False
        logger.info("-" * 50)
        logger.info("SHUTDOWN command received")
        return DiscoveryResponse(status=DiscoveryStatus.OK, message="Shutting down")

    def _cleanup(self) -> None:
        """Clean up resources."""
        if self._socket:
            try:
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

        logger.info("=" * 50)
        logger.info("DISCOVERY DAEMON STOPPED")
        logger.info("=" * 50)

    def stop(self) -> None:
        """Stop the discovery daemon."""
        logger.info("Stopping discovery daemon")
        self._running = False


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
        "--log-level",
        default="INFO",
        choices=["DEBUG", "INFO", "WARNING", "ERROR"],
        help="Logging level (default: INFO)",
    )

    args = parser.parse_args()

    # Set log level
    set_log_level(logger, args.log_level)

    # Create and run daemon
    daemon = DiscoveryDaemon(address=args.address)

    daemon.start()


if __name__ == "__main__":
    main()
