"""
Node abstraction for Cortex.

Provides an async interface for managing publishers and subscribers.
Uses asyncio for cooperative multitasking - ideal for Python < 3.14.
"""

import asyncio
import logging
import threading
from collections.abc import Callable
from typing import Literal

import zmq
import zmq.asyncio

from cortex.core.executor import RateExecutor
from cortex.core.publisher import Publisher
from cortex.core.subscriber import Subscriber
from cortex.core.sync_subscriber import SyncMessageCallback, ThreadedSubscriber
from cortex.core.types import AsyncCallback, MessageCallback
from cortex.discovery.daemon import DEFAULT_DISCOVERY_ADDRESS
from cortex.messages.base import Message

SubscriberMode = Literal["async", "sync"]
PublisherMode = Literal["async", "sync"]

logger = logging.getLogger("cortex.node")


class Node:
    """User-facing composition unit that owns publishers, subscribers, and timers.

    A node bundles a shared :class:`zmq.asyncio.Context`, a collection of
    :class:`cortex.core.publisher.Publisher` and
    :class:`cortex.core.subscriber.Subscriber` instances created through it,
    and any number of periodic timer callbacks.

    :meth:`run` starts every subscriber receive loop and every timer as
    asyncio tasks and ``gather``s them until cancelled. Use as an async
    context manager so that :meth:`close` runs on exit and cleans up
    sockets, tasks, and the shared ZMQ context.

    Example:
        ```python
        class CameraNode(Node):
            def __init__(self):
                super().__init__("camera_node")
                self.pub = self.create_publisher("/camera/image", ImageMessage)
                self.create_timer(1 / 30, self.publish_image)

            async def publish_image(self):
                self.pub.publish(ImageMessage(data=capture_image()))

        async def main():
            async with CameraNode() as node:
                await node.run()
        ```
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

        # Publishers and subscribers (async and sync share one keyed dict)
        self._publishers: dict[str, Publisher] = {}
        self._subscribers: dict[str, Subscriber | ThreadedSubscriber] = {}

        # Timer executors: (period, callback, RateExecutor)
        self._timers: list[tuple[float, AsyncCallback, RateExecutor]] = []

        # Async subscribers with callbacks need their receive loop scheduled
        # as an asyncio task; sync subscribers run on their own OS thread and
        # are tracked separately so close() can join them deterministically.
        self._active_subscribers: list[Subscriber] = []
        self._sync_subscribers: list[ThreadedSubscriber] = []

        # Independent zmq contexts created for sync-mode publishers; we
        # own their lifecycle and term them on close().
        self._owned_pub_contexts: list[zmq.Context] = []

        # Sync-side worker threads spawned via ``spawn_thread``. They share
        # one ``threading.Event`` for shutdown so ``stop()`` can signal all
        # of them at once and ``close()`` can join them deterministically.
        self._sync_stop_event = threading.Event()
        self._spawned_threads: list[threading.Thread] = []

        # Tasks
        self._tasks: list[asyncio.Task] = []

        # State
        self._running = False
        self._stop_event: asyncio.Event | None = None

        logger.info(f"Created node: {name}")

    def create_publisher(
        self,
        topic_name: str,
        message_type: type[Message],
        queue_size: int = 10,
        mode: PublisherMode = "async",
    ) -> Publisher:
        """
        Create a publisher for a topic.

        Args:
            topic_name: Name of the topic.
            message_type: Type of messages to publish.
            queue_size: Output queue size.
            mode: ``'async'`` (default) shares the node's
                :class:`zmq.asyncio.Context` (with a sync shadow). ``'sync'``
                gives the publisher its own independent
                :class:`zmq.Context` so ``publish()`` does not bounce
                through asyncio's IO threads — recommended for control-loop
                publishers calling ``publish()`` from a non-asyncio thread.
                Note that :class:`zmq.PUB` sockets are not thread-safe;
                only call ``publish()`` from one thread per Publisher.

        Returns:
            Publisher instance
        """
        if topic_name in self._publishers:
            logger.warning(f"Publisher for {topic_name} already exists")
            return self._publishers[topic_name]

        if mode == "async":
            pub_context = self._context
        elif mode == "sync":
            pub_context = zmq.Context()
            self._owned_pub_contexts.append(pub_context)
        else:
            raise ValueError(f"Unknown publisher mode: {mode!r}")

        pub = Publisher(
            topic_name=topic_name,
            message_type=message_type,
            node_name=self.name,
            discovery_address=self.discovery_address,
            queue_size=queue_size,
            context=pub_context,
        )

        self._publishers[topic_name] = pub
        logger.info("Created %s publisher for %s", mode, topic_name)

        return pub

    def create_subscriber(
        self,
        topic_name: str,
        message_type: type[Message],
        callback: MessageCallback | SyncMessageCallback | None = None,
        queue_size: int = 10,
        wait_for_topic: bool = True,
        topic_timeout: float = 30.0,
        mode: SubscriberMode = "async",
        strict_fingerprint: bool | None = None,
        cpu_affinity: list[int] | None = None,
        sched_priority: int | None = None,
    ) -> Subscriber | ThreadedSubscriber:
        """
        Create a subscriber for a topic.

        Args:
            topic_name: Name of the topic.
            message_type: Type of messages expected.
            callback: Function to call when messages arrive. ``mode='async'``
                expects an async callback; ``mode='sync'`` expects a plain
                synchronous callable and rejects coroutine functions.
            queue_size: Input queue size (ignored when ``conflate=True`` in
                sync mode).
            wait_for_topic: Whether to wait for the topic to be available.
            topic_timeout: Timeout for waiting for topic, in seconds.
            mode: ``'async'`` (default) routes through asyncio. ``'sync'``
                runs a dedicated OS thread with synchronous zmq + Poller —
                use for control loops needing tight p99 latency. In sync
                mode the default ``queue_size`` of ``1`` gives latest-wins
                semantics suitable for control commands.
            strict_fingerprint: When True, a fingerprint mismatch between
                the topic and ``message_type`` raises ``MessageFingerprintError``
                instead of logging a warning. Default behavior is mode-
                dependent: ``True`` in sync mode, ``False`` in async mode
                (kept lax for backward compatibility). Pass ``True``
                explicitly on async control topics where silent type
                confusion would corrupt downstream state.
            cpu_affinity: Sync mode only. Pin the receive thread to the
                given CPU set (Linux only; ignored elsewhere).
            sched_priority: Sync mode only. Run the receive thread under
                ``SCHED_FIFO`` at the given priority (Linux only; requires
                ``CAP_SYS_NICE``). Failure is logged and the thread falls
                back to the default scheduler.

        Returns:
            ``Subscriber`` for ``mode='async'``, ``ThreadedSubscriber`` for
            ``mode='sync'``.
        """
        if topic_name in self._subscribers:
            logger.warning(f"Subscriber for {topic_name} already exists")
            return self._subscribers[topic_name]

        if mode == "async":
            # Async default: lax (logs and continues) for compatibility.
            # Callers opt into strict via strict_fingerprint=True.
            async_strict = False if strict_fingerprint is None else strict_fingerprint
            sub: Subscriber | ThreadedSubscriber = Subscriber(
                topic_name=topic_name,
                message_type=message_type,
                callback=callback,
                node_name=self.name,
                discovery_address=self.discovery_address,
                queue_size=queue_size,
                wait_for_topic=wait_for_topic,
                topic_timeout=topic_timeout,
                context=self._context,
                strict_fingerprint=async_strict,
            )
            if callback is not None:
                self._active_subscribers.append(sub)
        elif mode == "sync":
            if callback is None:
                raise ValueError("Sync subscribers require a callback")
            if strict_fingerprint is False:
                # Allow callers to relax sync mode if they really mean it,
                # but the ThreadedSubscriber currently hard-codes strict.
                # Surface the override expectation as a clear log line so
                # the future relaxation is discoverable.
                logger.info(
                    "strict_fingerprint=False ignored for sync subscriber "
                    "%s; sync mode is always strict.",
                    topic_name,
                )
            sub = ThreadedSubscriber(
                topic_name=topic_name,
                message_type=message_type,
                callback=callback,  # type: ignore[arg-type]
                node_name=self.name,
                discovery_address=self.discovery_address,
                queue_size=queue_size,
                wait_for_topic=wait_for_topic,
                topic_timeout=topic_timeout,
                cpu_affinity=cpu_affinity,
                sched_priority=sched_priority,
            )
            self._sync_subscribers.append(sub)
        else:
            raise ValueError(f"Unknown subscriber mode: {mode!r}")

        self._subscribers[topic_name] = sub
        logger.info("Created %s subscriber for %s", mode, topic_name)
        return sub

    @property
    def stop_event(self) -> threading.Event:
        """Shared ``threading.Event`` set when the node is stopping.

        Sync code that opts into the node's lifecycle (publisher threads,
        I/O loops, anything spawned via :meth:`spawn_thread`) should poll
        ``node.stop_event.is_set()`` and exit promptly when it goes True.
        Async code should not need this — it gets cancellation through the
        normal asyncio task lifecycle.
        """
        return self._sync_stop_event

    def spawn_thread(
        self,
        target: Callable[..., None],
        *args,
        name: str | None = None,
        **kwargs,
    ) -> threading.Thread:
        """Start an OS thread owned by this node.

        ``target`` is invoked as ``target(stop_event, *args, **kwargs)`` —
        the first positional argument is always the node's shared
        ``threading.Event``. The thread is started immediately, registered
        for ``run()`` keepalive (so the asyncio side won't fall through),
        and joined deterministically by :meth:`close`.

        This is the canonical way to drive sync-mode publishers, custom
        polling loops, or any blocking I/O the node should manage.

        Args:
            target: The thread body. Must accept the stop event as its
                first positional arg.
            *args: Forwarded to ``target`` after the stop event.
            name: Thread name; defaults to ``"<node-name>-thread-<n>"``.
            **kwargs: Forwarded to ``target``.

        Returns:
            The :class:`threading.Thread` instance, already running.

        Example:
            ```python
            def control_loop(stop, pub, rate_hz):
                interval = 1.0 / rate_hz
                next_t = time.perf_counter()
                while not stop.is_set():
                    ...
                    pub.publish(WheelCommand(...))
                    next_t += interval
                    time.sleep(max(0, next_t - time.perf_counter()))

            pub = node.create_publisher(..., mode="sync")
            node.spawn_thread(control_loop, pub, 1000.0)
            await node.run()  # blocks until Ctrl+C; close() joins the thread
            ```
        """
        thread_name = name or f"{self.name}-thread-{len(self._spawned_threads)}"
        stop = self._sync_stop_event

        def _runner() -> None:
            try:
                target(stop, *args, **kwargs)
            except Exception:
                logger.exception("Spawned thread %s crashed", thread_name)

        thread = threading.Thread(target=_runner, name=thread_name, daemon=False)
        thread.start()
        self._spawned_threads.append(thread)
        logger.info("Spawned thread %s", thread_name)
        return thread

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

    async def run(self) -> None:
        """
        Run the node, processing messages and timers.

        This is the main async entry point for the node. Sync subscribers
        are started on their own OS threads and run independently of the
        asyncio event loop.
        """
        self._running = True

        # Start sync subscribers first — they don't depend on the loop and
        # we want them receiving as early as possible.
        for sub in self._sync_subscribers:
            sub.start()

        # Start all timer executors
        for _period, _callback, executor in self._timers:
            self._tasks.append(asyncio.create_task(executor.run()))

        # Start all async subscriber receive loops
        for sub in self._active_subscribers:
            self._tasks.append(asyncio.create_task(sub.run()))

        # If the node has no async work but does have sync work to manage
        # (sync subscribers and/or threads spawned via spawn_thread), keep
        # run() alive so the asyncio side does not fall through and trip
        # the finally block. Released by stop() / close().
        has_sync_work = bool(self._sync_subscribers) or bool(self._spawned_threads)
        if not self._tasks and has_sync_work:
            self._stop_event = asyncio.Event()
            self._tasks.append(asyncio.create_task(self._stop_event.wait()))

        logger.info(
            "Node %s running with %d async tasks, %d sync threads",
            self.name,
            len(self._tasks),
            len(self._sync_subscribers),
        )

        try:
            await asyncio.gather(*self._tasks, return_exceptions=True)
        except asyncio.CancelledError:
            logger.info(f"Node {self.name} cancelled")
        finally:
            self._running = False
            # Stop all executors
            for _period, _callback, executor in self._timers:
                executor.stop()
            for sub in self._active_subscribers:
                sub.stop()
            for sub in self._sync_subscribers:
                sub.stop()

    def stop(self) -> None:
        """Stop the node."""
        logger.info(f"Stopping node {self.name}")
        self._running = False

        # Stop all executors
        for _period, _callback, executor in self._timers:
            executor.stop()
        for sub in self._active_subscribers:
            sub.stop()
        for sub in self._sync_subscribers:
            sub.stop()

        # Signal all spawned sync threads to wind down.
        self._sync_stop_event.set()

        # Release the keepalive task (if any) so run() can return cleanly.
        if self._stop_event is not None and not self._stop_event.is_set():
            self._stop_event.set()

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

        # Close all subscribers (joins sync receive threads)
        for sub in self._subscribers.values():
            sub.close()
        self._subscribers.clear()

        # Join spawned sync worker threads — stop() already set the event.
        for thread in self._spawned_threads:
            thread.join(timeout=2.0)
            if thread.is_alive():
                logger.warning(
                    "Spawned thread %s did not exit within 2.0s", thread.name
                )
        self._spawned_threads.clear()

        self._timers.clear()
        self._active_subscribers.clear()
        self._sync_subscribers.clear()

        # Terminate ZMQ contexts: shared async first, then any sync contexts
        # created for sync-mode publishers.
        self._context.term()
        for ctx in self._owned_pub_contexts:
            try:
                ctx.term()
            except Exception as exc:
                logger.debug("Error terming sync publisher context: %s", exc)
        self._owned_pub_contexts.clear()

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

    # ------------------------------------------------------------------
    # Sync entry points — for nodes that only own sync work
    # ------------------------------------------------------------------

    def _has_async_work(self) -> bool:
        """True if the node has anything that needs an asyncio loop."""
        return bool(self._timers) or bool(self._active_subscribers)

    def spin(self, timeout: float | None = None) -> None:
        """Block the calling thread until the node is stopped.

        Sync counterpart to :meth:`run`. Use this when the node owns only
        sync work — sync subscribers, threads spawned via
        :meth:`spawn_thread`, or nothing more than a publisher driven from
        the calling thread itself. No asyncio loop is created.

        Raises ``RuntimeError`` if the node has async timers or async
        subscribers, since those need :meth:`run` to be scheduled. ``Ctrl+C``
        is delivered as :class:`KeyboardInterrupt` and propagates so the
        caller can decide whether to swallow it.

        Args:
            timeout: Optional cap (seconds) on how long to block. ``None``
                means "wait forever, until :meth:`stop` is called".

        Example:
            ```python
            node = Node("controller")
            pub = node.create_publisher("/cmd", WheelCommand, mode="sync")
            node.spawn_thread(control_loop, pub, 1000.0)
            try:
                node.spin()              # blocks until Ctrl+C
            except KeyboardInterrupt:
                pass
            finally:
                node.close_sync()
            ```
        """
        if self._has_async_work():
            raise RuntimeError(
                "Node.spin() does not start an asyncio loop, but this node "
                "has async timers/subscribers. Use `await node.run()` instead, "
                "or remove the async work."
            )

        self._running = True
        for sub in self._sync_subscribers:
            sub.start()

        logger.info(
            "Node %s spinning with %d sync subscribers and %d threads",
            self.name,
            len(self._sync_subscribers),
            len(self._spawned_threads),
        )
        try:
            # ``Event.wait`` is interruptible by Ctrl+C on the main thread.
            self._sync_stop_event.wait(timeout=timeout)
        finally:
            self._running = False
            for sub in self._sync_subscribers:
                sub.stop()

    def close_sync(self) -> None:
        """Sync counterpart to :meth:`close`.

        Tears down sockets, joins spawned threads, and terms zmq contexts
        without ever entering an asyncio loop. Safe to call from a plain
        ``def main()`` — including from inside ``__exit__`` when the node
        is used as a regular ``with`` context manager.

        Will refuse to run if the node has async timers/subscribers; for
        those, use ``await node.close()``.
        """
        if self._has_async_work():
            raise RuntimeError(
                "Node.close_sync() cannot tear down async timers/subscribers. "
                "Use `await node.close()` instead."
            )

        logger.info("Closing node %s (sync)", self.name)

        # Signal everyone, then synchronously join.
        self._sync_stop_event.set()
        for sub in self._sync_subscribers:
            sub.stop()
        self._running = False

        # Close publishers and (sync) subscribers.
        for pub in self._publishers.values():
            pub.close()
        self._publishers.clear()
        for sub in self._subscribers.values():
            sub.close()
        self._subscribers.clear()

        # Join spawned worker threads.
        for thread in self._spawned_threads:
            thread.join(timeout=2.0)
            if thread.is_alive():
                logger.warning(
                    "Spawned thread %s did not exit within 2.0s", thread.name
                )
        self._spawned_threads.clear()

        self._sync_subscribers.clear()

        # Term zmq contexts. The shared async context is never used by a
        # purely-sync node, but term it anyway so leaks don't accumulate.
        try:
            self._context.term()
        except Exception as exc:
            logger.debug("Error terming async context: %s", exc)
        for ctx in self._owned_pub_contexts:
            try:
                ctx.term()
            except Exception as exc:
                logger.debug("Error terming sync publisher context: %s", exc)
        self._owned_pub_contexts.clear()

        logger.info("Node %s closed", self.name)

    # ------------------------------------------------------------------
    # Context managers
    # ------------------------------------------------------------------

    def __enter__(self) -> "Node":
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self.close_sync()

    async def __aenter__(self) -> "Node":
        return self

    async def __aexit__(self, exc_type, exc_val, exc_tb) -> None:
        await self.close()
