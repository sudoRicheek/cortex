"""
Asynchronous subscriber implementation.

Builds on :class:`cortex.core.subscriber_base.SubscriberBase` and pulls
frames off the wire through ``zmq.asyncio``. Use this for the common
case — telemetry, dashboards, anything that lives inside an asyncio
event loop. For control-loop topics that need <100 µs p99, see
:class:`cortex.core.sync_subscriber.ThreadedSubscriber`.
"""

import asyncio
import contextlib
import logging
import time
from time import perf_counter_ns
from typing import Any

import zmq
import zmq.asyncio

from cortex.core.executor import AsyncExecutor
from cortex.core.subscriber_base import (
    MessageFingerprintError,
    SubscriberBase,
    decode_frames,
    update_stats_for_header,
)
from cortex.core.types import MessageCallback
from cortex.discovery.daemon import DEFAULT_DISCOVERY_ADDRESS
from cortex.messages.base import Message, MessageHeader
from cortex.utils import tracing

logger = logging.getLogger("cortex.subscriber")


class Subscriber(SubscriberBase):
    """Async subscriber: receives typed messages on a topic from a ZMQ SUB socket.

    On construction, the subscriber performs a non-blocking lookup against
    the discovery daemon. If the topic already has a publisher it connects
    immediately; otherwise it defers and retries with an async polling
    wait inside :meth:`run`.

    When constructed with a ``callback`` the subscriber drives its own
    receive loop (one task, one callback at a time — see
    :class:`cortex.core.executor.AsyncExecutor`). Without a callback the
    subscriber is passive and the caller polls via :meth:`receive`.

    Always create via :meth:`Node.create_subscriber`.
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
        strict_fingerprint: bool = False,
    ):
        super().__init__(
            topic_name=topic_name,
            message_type=message_type,
            node_name=node_name,
            discovery_address=discovery_address,
            topic_timeout=topic_timeout,
            wait_for_topic=wait_for_topic,
            strict_fingerprint=strict_fingerprint,
        )
        self._callback = callback
        self.queue_size = queue_size

        self._context: zmq.asyncio.Context = context or zmq.asyncio.Context()
        self._socket: zmq.asyncio.Socket | None = None

        # Compatibility shim: legacy code reads ``_last_receive_time`` directly.
        self._last_receive_time: float | None = None

        self._executor: AsyncExecutor | None = None

        # Try non-blocking connect (will succeed if topic already exists)
        if self._lookup_nonblocking():
            self._setup_socket(self._topic_info.address)
            self._connected = True
            logger.info(
                "Connected to topic %s at %s", self.topic_name, self._topic_info.address
            )
        else:
            logger.warning(
                "Topic %s not found yet, will retry in run()", self.topic_name
            )

    async def _async_connect(self) -> bool:
        """Async wait for the topic and connect once available."""
        if self._connected:
            return True
        try:
            if self._wait_for_topic:
                logger.info("Waiting for topic %s...", self.topic_name)
                self._topic_info = await self._discovery_client.wait_for_topic_async(
                    self.topic_name, timeout=self.topic_timeout
                )
            else:
                self._topic_info = self._discovery_client.lookup_topic(self.topic_name)
        except Exception as exc:
            logger.error("Failed to connect to topic: %s", exc)
            return False

        if self._topic_info is None:
            return False
        try:
            self._validate_fingerprint(self._topic_info)
        except MessageFingerprintError:
            raise

        self._setup_socket(self._topic_info.address)
        self._connected = True
        logger.info(
            "Connected to topic %s at %s", self.topic_name, self._topic_info.address
        )
        return True

    def _setup_socket(self, address: str) -> None:
        """Create the SUB socket, set HWM/topic filter, and connect."""
        self._socket = self._context.socket(zmq.SUB)
        self._socket.setsockopt(zmq.RCVHWM, self.queue_size)
        self._socket.setsockopt(zmq.LINGER, 0)
        self._socket.setsockopt_string(zmq.SUBSCRIBE, self.topic_name)
        self._socket.connect(address)
        logger.debug("Subscriber socket connected to %s", address)

    async def receive(self) -> tuple[Message, MessageHeader] | None:
        if not self._connected or self._socket is None:
            return None

        try:
            with tracing.stage("async.recv_multipart"):
                frames = await self._socket.recv_multipart(copy=False)

            with tracing.stage("async.decode"):
                decoded = decode_frames(self.message_type, frames)
            if decoded is None:
                return None
            message, header = decoded

            update_stats_for_header(self.stats, header, perf_counter_ns())
            self._last_receive_time = time.time()
            return message, header

        except asyncio.CancelledError:
            raise
        except Exception as exc:
            logger.error("Failed to receive message: %s", exc)
            return None

    async def _receive_and_callback(self) -> Any:
        result = await self.receive()
        if result is None:
            return None
        message, header = result
        with tracing.stage("async.callback"):
            return await self._callback(message, header)

    def start(self) -> None:
        if self._executor:
            self._executor.start()

    def stop(self) -> None:
        if self._executor:
            self._executor.stop()

    @property
    def running(self) -> bool:
        return self._executor.running if self._executor else False

    async def run(self) -> None:
        if self._callback is None:
            logger.warning("No callback set for subscriber %s", self.topic_name)
            return

        if not self._connected and not await self._async_connect():
            logger.error("Failed to connect subscriber for %s", self.topic_name)
            return

        logger.info("Subscriber for %s running", self.topic_name)
        self._executor = AsyncExecutor(self._receive_and_callback)
        await self._executor.run()
        logger.info("Subscriber for %s stopped", self.topic_name)

    @property
    def last_receive_time(self) -> float | None:
        return self._last_receive_time

    def close(self) -> None:
        logger.info("Closing subscriber for %s", self.topic_name)
        if self._executor:
            self._executor.stop()
            self._executor = None

        self._close_discovery()

        if self._socket:
            with contextlib.suppress(Exception):
                self._socket.close()
            self._socket = None

        self._connected = False


# Public alias — callers that opt in to the explicit naming get it; the
# default ``Subscriber`` import path stays where it has always been.
AsyncSubscriber = Subscriber
