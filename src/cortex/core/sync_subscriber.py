"""
Thread-backed synchronous subscriber for low-latency control topics.

The async subscriber goes through ``zmq.asyncio`` and the asyncio event
loop — that costs ~4 ``await`` boundaries per message and limits p99 to
roughly 1 ms even on inproc/IPC. For control loops at >100 Hz where
jitter matters, this implementation runs a dedicated OS thread that
pulls frames synchronously through a ``zmq.Poller`` and dispatches to a
**sync** user callback inline.

On a free-threaded build of CPython (``python3.14t``, PEP 779) the
receive thread does not contend with the asyncio thread for the GIL,
which is what makes the <100 µs p99 target reachable. The class works
on stock CPython too — just with a higher floor — and emits a one-line
runtime hint when it detects a GIL-enabled interpreter so users know
the upgrade exists.

Public API mirrors :class:`cortex.core.subscriber.Subscriber` where it
makes sense (topic name, message type, callback, queue size, discovery
plumbing) but diverges in two important ways:

1. The callback **must be synchronous**. Awaiting on a worker thread
   would re-introduce the per-await scheduling cost we're trying to
   escape. A clear :class:`TypeError` is raised at construction time if
   a coroutine function is passed.
2. By default ``queue_size=1``. For control commands you want the latest
   message, never a queued backlog. Note that ZMQ's ``CONFLATE`` socket
   option *cannot* be used here — it strips multipart messages, and
   Cortex publishers always send multipart frames. ``RCVHWM=1`` gives
   the equivalent "drop old, keep newest" effect on the receiver while
   preserving the wire format.
"""

import contextlib
import inspect
import logging
import os
import threading
from collections.abc import Callable
from time import perf_counter_ns

import zmq

from cortex.core.subscriber_base import (
    SubscriberBase,
    decode_frames,
    update_stats_for_header,
)
from cortex.discovery.daemon import DEFAULT_DISCOVERY_ADDRESS
from cortex.messages.base import Message, MessageHeader
from cortex.utils import tracing
from cortex.utils.runtime import is_free_threaded, low_latency_advisory

logger = logging.getLogger("cortex.subscriber.sync")

SyncMessageCallback = Callable[[Message, MessageHeader], None]
"""A blocking callback invoked on the receive thread — must not return a coroutine."""


class ThreadedSubscriber(SubscriberBase):
    """Synchronous SUB-side receive loop running on a dedicated OS thread.

    Lifecycle:

    * Construction blocks on a discovery lookup (with optional wait), opens
      a fresh sync ``zmq.Context``, and connects the SUB socket.
      Construction does **not** start the worker thread.
    * :meth:`start` spins up the thread; the thread blocks in
      ``poller.poll(timeout_ms)`` between messages so shutdown is prompt.
    * :meth:`stop` signals the thread and joins it (with a 1 s default
      grace period); :meth:`close` calls :meth:`stop` and tears down zmq.

    The class is reentrant-safe in the trivial sense that ``start`` /
    ``stop`` / ``close`` are idempotent. ``zmq.SUB`` itself is single-
    threaded; do not call :meth:`receive` from another thread while the
    worker is running.
    """

    _POLL_TIMEOUT_MS = 50  # bound on shutdown latency
    _JOIN_TIMEOUT_S = 1.0

    def __init__(
        self,
        topic_name: str,
        message_type: type[Message],
        callback: SyncMessageCallback,
        node_name: str = "anonymous",
        discovery_address: str = DEFAULT_DISCOVERY_ADDRESS,
        queue_size: int = 1,
        wait_for_topic: bool = True,
        topic_timeout: float = 30.0,
        cpu_affinity: list[int] | None = None,
        sched_priority: int | None = None,
    ):
        # Strict fingerprint by default in sync mode: callers picked sync
        # for predictability, so silent type-confusion is unacceptable.
        super().__init__(
            topic_name=topic_name,
            message_type=message_type,
            node_name=node_name,
            discovery_address=discovery_address,
            topic_timeout=topic_timeout,
            wait_for_topic=wait_for_topic,
            strict_fingerprint=True,
        )

        if inspect.iscoroutinefunction(callback):
            raise TypeError(
                "ThreadedSubscriber requires a *synchronous* callback. "
                "Pass an async callback through Node.create_subscriber(mode='async') "
                "instead."
            )

        self._callback = callback
        self._queue_size = queue_size
        self._cpu_affinity = cpu_affinity
        self._sched_priority = sched_priority

        self._context = zmq.Context()
        self._socket: zmq.Socket | None = None
        self._poller: zmq.Poller | None = None

        self._thread: threading.Thread | None = None
        self._stop_event = threading.Event()
        self._started = False

        # Resolve the topic and open the socket up front, so construction
        # failures surface to the caller (not the worker thread).
        if not self._lookup_blocking():
            raise TimeoutError(
                f"Topic {self.topic_name} not registered with discovery within "
                f"{self.topic_timeout}s"
            )
        self._setup_socket(self._topic_info.address)
        self._connected = True

        advisory = low_latency_advisory()
        if advisory:
            logger.info(advisory)

    # ------------------------------------------------------------------ socket

    def _setup_socket(self, address: str) -> None:
        sock = self._context.socket(zmq.SUB)
        # RCVHWM gives "drop old, keep newest" semantics on overflow — the
        # right default for control topics. We deliberately do NOT use
        # ZMQ_CONFLATE; it is incompatible with multipart messages and
        # would silently strip every frame except the last.
        sock.setsockopt(zmq.RCVHWM, max(self._queue_size, 1))
        sock.setsockopt(zmq.LINGER, 0)
        sock.setsockopt_string(zmq.SUBSCRIBE, self.topic_name)
        sock.connect(address)
        self._socket = sock

        poller = zmq.Poller()
        poller.register(sock, zmq.POLLIN)
        self._poller = poller

    # ------------------------------------------------------------------ thread

    def start(self) -> None:
        """Spin up the receive thread (idempotent)."""
        if self._started:
            return
        if not self._connected:
            raise RuntimeError(
                f"Subscriber {self.topic_name} is not connected; cannot start"
            )
        self._stop_event.clear()
        self._thread = threading.Thread(
            target=self._run,
            name=f"cortex-sub-{self.topic_name}",
            daemon=False,
        )
        self._thread.start()
        self._started = True

    def stop(self, timeout: float | None = None) -> None:
        """Signal the worker and join it (idempotent)."""
        if not self._started:
            return
        self._stop_event.set()
        if self._thread is not None:
            self._thread.join(timeout if timeout is not None else self._JOIN_TIMEOUT_S)
            if self._thread.is_alive():
                logger.warning(
                    "ThreadedSubscriber for %s did not stop within %.1fs",
                    self.topic_name,
                    timeout if timeout is not None else self._JOIN_TIMEOUT_S,
                )
        self._thread = None
        self._started = False

    @property
    def running(self) -> bool:
        return self._started and self._thread is not None and self._thread.is_alive()

    # ------------------------------------------------------------------ loop

    def _apply_thread_tuning(self) -> None:
        """Apply CPU affinity and (if requested) real-time scheduling.

        Both are best-effort: we log a warning and continue on any failure
        (missing capability, non-Linux platform, EPERM). The receive loop
        works without either knob — they only buy lower jitter.
        """
        if self._cpu_affinity is not None:
            sched_setaffinity = getattr(os, "sched_setaffinity", None)
            if sched_setaffinity is None:
                logger.warning(
                    "CPU affinity requested but not supported on this platform"
                )
            else:
                try:
                    sched_setaffinity(0, set(self._cpu_affinity))
                    logger.info(
                        "Pinned receive thread to CPUs %s",
                        sorted(self._cpu_affinity),
                    )
                except OSError as exc:
                    logger.warning("Failed to set CPU affinity: %s", exc)

        if self._sched_priority is not None:
            sched_setscheduler = getattr(os, "sched_setscheduler", None)
            sched_param_cls = getattr(os, "sched_param", None)
            sched_fifo = getattr(os, "SCHED_FIFO", None)
            if (
                sched_setscheduler is None
                or sched_param_cls is None
                or sched_fifo is None
            ):
                logger.warning(
                    "SCHED_FIFO requested but not supported on this platform"
                )
                return
            try:
                sched_setscheduler(0, sched_fifo, sched_param_cls(self._sched_priority))
                logger.info(
                    "Receive thread set to SCHED_FIFO at priority %d",
                    self._sched_priority,
                )
            except (OSError, PermissionError) as exc:
                # Most common failure mode: missing CAP_SYS_NICE. Don't bail —
                # the receive loop still works on the default scheduler.
                logger.warning(
                    "Failed to set SCHED_FIFO priority %d (need CAP_SYS_NICE): %s",
                    self._sched_priority,
                    exc,
                )

    def _run(self) -> None:
        """Worker thread entry point."""
        self._apply_thread_tuning()
        if is_free_threaded():
            logger.debug(
                "Sync subscriber %s on free-threaded interpreter (no GIL contention)",
                self.topic_name,
            )

        sock = self._socket
        poller = self._poller
        assert sock is not None and poller is not None

        timeout_ms = self._POLL_TIMEOUT_MS
        try:
            while not self._stop_event.is_set():
                events = dict(poller.poll(timeout=timeout_ms))
                if sock not in events:
                    continue

                with tracing.stage("sync.recv_multipart"):
                    try:
                        frames = sock.recv_multipart(copy=False, flags=zmq.NOBLOCK)
                    except zmq.Again:
                        continue

                with tracing.stage("sync.decode"):
                    decoded = decode_frames(self.message_type, frames)
                if decoded is None:
                    continue
                message, header = decoded

                update_stats_for_header(self.stats, header, perf_counter_ns())

                with tracing.stage("sync.callback"):
                    try:
                        self._callback(message, header)
                    except Exception as exc:
                        # Don't kill the receive thread on a user error.
                        logger.exception(
                            "Callback raised on topic %s: %s", self.topic_name, exc
                        )
        except Exception:
            logger.exception(
                "ThreadedSubscriber receive loop crashed for %s", self.topic_name
            )

    # ------------------------------------------------------------------ shutdown

    def close(self) -> None:
        """Stop the worker and tear down zmq state (idempotent)."""
        logger.info("Closing sync subscriber for %s", self.topic_name)
        self.stop()

        self._close_discovery()

        if self._socket is not None:
            with contextlib.suppress(Exception):
                self._socket.close()
            self._socket = None
        self._poller = None

        with contextlib.suppress(Exception):
            self._context.term()
        self._connected = False

    # ------------------------------------------------------------------ stats

    @property
    def is_running(self) -> bool:
        return self.running
