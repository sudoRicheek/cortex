"""
Executor for managing async functions at constant rates.

Provides utilities for executing async callbacks with precise timing,
faithful to Python's cooperative multitasking model.
"""

import asyncio
import logging
import threading
import time
from abc import ABC, abstractmethod
from collections.abc import Callable

from cortex.core.types import AsyncCallback

logger = logging.getLogger("cortex.executor")


class BaseAsyncExecutor(ABC):
    """
    Abstract base class for async executors.

    Provides common interface for starting, stopping, and running
    async callback functions.
    """

    def __init__(self, func: AsyncCallback):
        """
        Initialize the executor.

        Args:
            func: Async function to execute
        """
        self.func = func
        self._running = False

    @property
    def running(self) -> bool:
        """Check if the executor is running."""
        return self._running

    def start(self) -> None:
        """Start the executor."""
        self._running = True

    def stop(self) -> None:
        """Stop the executor."""
        self._running = False

    async def run(self, *args, **kwargs) -> None:
        """Run the executor."""
        self.start()
        try:
            await self._run_impl(*args, **kwargs)
        finally:
            self.stop()

    @abstractmethod
    async def _run_impl(self, *args, **kwargs) -> None:
        """Implementation of the run loop. Subclasses must override."""
        ...


class AsyncExecutor(BaseAsyncExecutor):
    """Runs an async callable in a tight loop, yielding to the event loop.

    Used by :class:`cortex.core.subscriber.Subscriber` to drive its
    receive → decode → dispatch loop. Exceptions are logged and the loop
    continues; only :class:`asyncio.CancelledError` stops it.

    Example:
        ```python
        async def process_data():
            data = await get_data()
            await handle(data)

        executor = AsyncExecutor(process_data)
        await executor.run()
        ```
    """

    async def _run_impl(self, *args, **kwargs) -> None:
        """Run the async function as fast as possible."""
        while self._running:
            try:
                await self.func(*args, **kwargs)
                await asyncio.sleep(0)  # Yield to event loop
            except asyncio.CancelledError:
                break
            except Exception as e:
                logger.error(f"Error in AsyncExecutor: {e}")
                await asyncio.sleep(0)


class RateExecutor(BaseAsyncExecutor):
    """Runs an async callable at a target rate in Hz.

    Uses ``time.perf_counter`` for scheduling. If a callback overruns the
    nominal period, ``next_exec_time`` stays on the fixed grid (only
    ``+ interval`` per invocation); the loop then sleeps 0 until the clock
    catches up, so **missed ticks are not skipped**. This matches the
    historical neurosim ``ZMQNODE`` constant-rate executor behavior and is
    appropriate for simulation stepping.

    Example:
        ```python
        async def my_callback():
            print("tick")

        executor = RateExecutor(my_callback, rate_hz=10.0)
        await executor.run()
        ```
    """

    def __init__(self, func: AsyncCallback, rate_hz: float):
        """
        Initialize constant rate executor.

        Args:
            func: Async function to execute
            rate_hz: Target execution rate in Hz
        """
        super().__init__(func)
        self._rate_hz = rate_hz
        self.interval = 1.0 / rate_hz

    async def _run_impl(self, *args, **kwargs) -> None:
        """
        Run a function on a fixed ``perf_counter`` grid at ``rate_hz``.

        When the callback is slow, ticks are not skipped: ``next_exec_time``
        advances by one interval per invocation and the loop yields until
        the clock catches up (zero-length sleeps while behind).
        """
        next_exec_time = time.perf_counter()

        while self._running:
            try:
                current_time = time.perf_counter()

                if current_time >= next_exec_time:
                    await self.func(*args, **kwargs)
                    next_exec_time += self.interval

                await asyncio.sleep(0)  # Yield to event loop
            except asyncio.CancelledError:
                break
            except Exception as e:
                logger.error(f"Error in RateExecutor: {e}")
                await asyncio.sleep(0)

            await asyncio.sleep(max(0, next_exec_time - time.perf_counter()))


class SyncRateExecutor:
    """Runs a plain (non-async) callable at a target rate in Hz, on a dedicated thread.

    The sync analogue of :class:`RateExecutor`. Scheduling matches
    ``RateExecutor`` exactly: a ``time.perf_counter`` grid with one
    ``+ interval`` advance per invocation, so **missed ticks are not
    skipped** when the callback overruns. Pair with
    :meth:`cortex.core.node.Node.create_timer` (``mode='sync'``) for
    control-loop work that should not pay the asyncio scheduler tax.

    The lifecycle mirrors :class:`BaseAsyncExecutor`: ``start`` /
    ``stop`` / ``run`` / ``running``. ``stop`` is thread-safe and sets
    an internal :class:`threading.Event` that the run loop sleeps on,
    so a sleeping run loop wakes immediately when another thread calls
    :meth:`stop` — no polling, no shared external event needed.

    Example:
        ```python
        def tick(*args):
            print("tick", args)

        executor = SyncRateExecutor(tick, rate_hz=1000.0)

        t = threading.Thread(target=executor.run, args=("hello",))
        t.start()
        ...
        executor.stop()
        t.join()
        ```
    """

    _MAX_SLEEP_S = 0.01

    def __init__(self, func: Callable[..., None], rate_hz: float):
        """
        Args:
            func: Plain (non-async) callable invoked once per tick. The
                ``*args`` / ``**kwargs`` passed to :meth:`run` are
                forwarded to ``func`` on every invocation.
            rate_hz: Target execution rate in Hz.
        """
        self.func = func
        self._rate_hz = rate_hz
        self.interval = 1.0 / rate_hz
        # Event is the single source of truth: set == stopped (the default
        # construction state), clear == running. Doubles as the
        # interruptible wait inside the run loop.
        self._stop_event = threading.Event()
        self._stop_event.set()

    @property
    def running(self) -> bool:
        """True between :meth:`start` (or :meth:`run`) and :meth:`stop`."""
        return not self._stop_event.is_set()

    def start(self) -> None:
        """Mark the executor as running.

        :meth:`run` calls this automatically; exposed for symmetry with
        :meth:`BaseAsyncExecutor.start` and to make restart-after-stop
        safe.
        """
        self._stop_event.clear()

    def stop(self) -> None:
        """Request the run loop to exit promptly.

        Thread-safe. A run loop currently sleeping wakes immediately.
        Idempotent.
        """
        self._stop_event.set()

    def run(self, *args, **kwargs) -> None:
        """Drive ``func(*args, **kwargs)`` at ``rate_hz`` until :meth:`stop`.

        Blocks the calling thread. ``*args`` / ``**kwargs`` are forwarded
        to ``self.func`` on every tick (matches
        :meth:`BaseAsyncExecutor.run`). Exceptions raised by the
        callback are logged and the loop continues; only :meth:`stop`
        exits cleanly.
        """
        self.start()
        next_exec_time = time.perf_counter()
        try:
            while not self._stop_event.is_set():
                if time.perf_counter() >= next_exec_time:
                    try:
                        self.func(*args, **kwargs)
                    except Exception as exc:
                        logger.error("Error in SyncRateExecutor: %s", exc)
                    next_exec_time += self.interval

                sleep = next_exec_time - time.perf_counter()
                # Sleep on the stop event so stop() wakes us immediately.
                # Cap each slice at _MAX_SLEEP_S so the loop also re-checks
                # ``perf_counter`` on schedule when the callback is slow.
                if sleep > 0 and self._stop_event.wait(min(sleep, self._MAX_SLEEP_S)):
                    break
        finally:
            self._stop_event.set()
