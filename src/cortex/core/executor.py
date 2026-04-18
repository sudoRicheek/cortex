"""
Executor for managing async functions at constant rates.

Provides utilities for executing async callbacks with precise timing,
faithful to Python's cooperative multitasking model.
"""

import asyncio
import logging
import time
from abc import ABC, abstractmethod

from cortex.core.types import AsyncCallback

logger = logging.getLogger("cortex.executor")


class BaseExecutor(ABC):
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


class AsyncExecutor(BaseExecutor):
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


class RateExecutor(BaseExecutor):
    """Runs an async callable at a target rate in Hz.

    Uses ``time.perf_counter`` for scheduling and catches up on overruns by
    advancing ``next_exec_time`` instead of firing back-to-back. Dropped
    ticks are **not** reported — suitable for telemetry and periodic I/O,
    but not for hard real-time control without external monitoring.

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
        Run a function at constant rate with precise timing.

        Executions happen at exact intervals regardless of execution time.
        """
        next_exec_time = time.perf_counter()

        while self._running:
            try:
                current_time = time.perf_counter()

                if current_time >= next_exec_time:
                    await self.func(*args, **kwargs)
                    next_exec_time += self.interval

                    # If we've fallen behind, catch up
                    if next_exec_time < current_time:
                        next_exec_time = current_time + self.interval

                await asyncio.sleep(0)  # Yield to event loop
            except asyncio.CancelledError:
                break
            except Exception as e:
                logger.error(f"Error in RateExecutor: {e}")
                await asyncio.sleep(0)

            await asyncio.sleep(max(0, next_exec_time - time.perf_counter()))
