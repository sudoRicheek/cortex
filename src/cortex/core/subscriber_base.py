"""
Shared subscriber primitives.

The async subscriber (``cortex.core.subscriber.Subscriber``) and the
threaded subscriber (``cortex.core.sync_subscriber.ThreadedSubscriber``)
diverge in *how* they pull frames off the wire — one ``await``-s a
``zmq.asyncio`` socket, the other blocks an OS thread on a
``zmq.Poller``. Everything **around** that — discovery lookup, type
fingerprint validation, frame decoding, stats, and per-publisher
sequence-gap detection — is identical and lives here.

This module owns no zmq sockets. It is pure dataflow + bookkeeping.
"""

import logging
from dataclasses import dataclass, field

from cortex.discovery.client import DiscoveryClient
from cortex.discovery.daemon import DEFAULT_DISCOVERY_ADDRESS
from cortex.discovery.protocol import TopicInfo
from cortex.messages.base import Message, MessageHeader

logger = logging.getLogger("cortex.subscriber")


class MessageFingerprintError(RuntimeError):
    """Raised when an incoming topic's fingerprint doesn't match the expected type."""


@dataclass
class SubscriberStats:
    """Per-subscriber counters; updated by the receive loop."""

    received: int = 0
    dropped_estimated: int = 0
    last_recv_perf_ns: int | None = None
    last_sequence_by_publisher: dict[int, int] = field(default_factory=dict)


def decode_frames(
    message_type: type[Message], frames: list[object]
) -> tuple[Message, MessageHeader] | None:
    """Decode ``[topic, header, metadata, *buffers]`` into a typed message.

    Returns ``None`` and logs a warning on malformed input rather than
    raising — the receive loop should not die on a single bad frame.
    """
    if len(frames) < 2:
        logger.warning("Unexpected frame count: %d", len(frames))
        return None

    payload_frames = frames[1:]
    try:
        if len(payload_frames) == 1:
            raw = (
                memoryview(payload_frames[0].buffer)
                if hasattr(payload_frames[0], "buffer")
                else payload_frames[0]
            )
            return message_type.from_bytes(raw)
        return message_type.from_frames(payload_frames)
    except Exception as exc:
        logger.error("Decode failed: %s", exc)
        return None


def update_stats_for_header(
    stats: SubscriberStats, header: MessageHeader, now_perf_ns: int
) -> int:
    """Bump receive counters and infer dropped messages from sequence gaps.

    Each ``Subscriber`` connects to exactly one topic, and each topic has a
    single publisher (today), so keying by ``fingerprint`` is effectively
    keying by ``(publisher, type)``. When multi-publisher fan-in lands we
    will extend the key to ``(publisher_node, fingerprint)``.

    Returns the number of dropped messages inferred from this header.
    """
    stats.received += 1
    stats.last_recv_perf_ns = now_perf_ns

    last = stats.last_sequence_by_publisher.get(header.fingerprint)
    stats.last_sequence_by_publisher[header.fingerprint] = header.sequence
    if last is None:
        return 0
    gap = header.sequence - last - 1
    if gap > 0:
        stats.dropped_estimated += gap
        return gap
    return 0


class SubscriberBase:
    """Discovery + connection scaffolding shared by all subscriber implementations.

    Subclasses are responsible only for the I/O loop. They set
    :attr:`_topic_info` via :meth:`_lookup_blocking` (or the async variant
    used by the asyncio subscriber) and then open whatever socket they
    prefer against :attr:`_topic_info.address`.
    """

    def __init__(
        self,
        topic_name: str,
        message_type: type[Message],
        node_name: str = "anonymous",
        discovery_address: str = DEFAULT_DISCOVERY_ADDRESS,
        topic_timeout: float = 600.0,
        wait_for_topic: bool = True,
        strict_fingerprint: bool = False,
    ):
        self.topic_name = topic_name
        self.message_type = message_type
        self.node_name = node_name
        self.discovery_address = discovery_address
        self.topic_timeout = topic_timeout
        self._wait_for_topic = wait_for_topic
        self._strict_fingerprint = strict_fingerprint

        self._topic_info: TopicInfo | None = None
        self._connected = False
        self._discovery_client: DiscoveryClient | None = DiscoveryClient(
            discovery_address=self.discovery_address
        )
        self.stats = SubscriberStats()

    # ------------------------------------------------------------------ discovery

    def _validate_fingerprint(self, info: TopicInfo) -> None:
        """Refuse or warn on type mismatch.

        Strict mode raises; lax mode preserves historical
        warning-and-continue behavior (kept until callers opt in).
        """
        expected = self.message_type.fingerprint()
        if info.fingerprint == expected:
            return
        msg = (
            f"Message type mismatch for {self.topic_name}: "
            f"expected {self.message_type.__name__} (fp={expected:#018x}), "
            f"got {info.message_type} (fp={info.fingerprint:#018x})"
        )
        if self._strict_fingerprint:
            raise MessageFingerprintError(msg)
        logger.warning(msg)

    def _lookup_nonblocking(self) -> bool:
        """One-shot lookup. Returns True on success."""
        try:
            self._topic_info = self._discovery_client.lookup_topic(self.topic_name)
        except Exception as exc:
            logger.error("Failed to lookup topic: %s", exc)
            return False
        if self._topic_info is None:
            return False
        self._validate_fingerprint(self._topic_info)
        return True

    def _lookup_blocking(self, poll_interval: float = 0.5) -> bool:
        """Block-and-poll for the topic up to :attr:`topic_timeout`."""
        try:
            self._topic_info = self._discovery_client.wait_for_topic(
                self.topic_name,
                timeout=self.topic_timeout,
                poll_interval=poll_interval,
            )
        except Exception as exc:
            logger.error("Failed to wait for topic: %s", exc)
            return False
        if self._topic_info is None:
            return False
        self._validate_fingerprint(self._topic_info)
        return True

    # ------------------------------------------------------------------ properties

    @property
    def is_connected(self) -> bool:
        return self._connected

    @property
    def topic_info(self) -> TopicInfo | None:
        return self._topic_info

    @property
    def receive_count(self) -> int:
        return self.stats.received

    @property
    def dropped_count(self) -> int:
        return self.stats.dropped_estimated

    # ------------------------------------------------------------------ shutdown

    def _close_discovery(self) -> None:
        if self._discovery_client is not None:
            try:
                self._discovery_client.close()
            except Exception as exc:  # best-effort
                logger.debug("Discovery close error: %s", exc)
            self._discovery_client = None
