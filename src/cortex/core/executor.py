"""
Executor for managing async functions at constant rates.

Provides a utility for executing async callbacks with precise timing,
faithful to Python's cooperative multitasking model.
"""

import asyncio
import logging
import time
from collections.abc import Coroutine
from typing import Any, Callable, Optional

logger = logging.getLogger("cortex.executor")


# Type alias for async callbacks
AsyncCallback = Callable[..., Coroutine[Any, Any, None]]


class RateExecutor:
    """
    Utility class for executing async functions at a constant rate.

    Provides precise timing for periodic execution of async callbacks.
    Uses cooperative multitasking - ideal for I/O-bound workloads in Python < 3.14.

    Example:
        async def my_callback():
            print("tick")

        executor = RateExecutor(my_callback, rate_hz=10.0)
        executor.start()
        await executor.run()
    """

    def __init__(
        self,
        func: AsyncCallback,
        rate_hz: Optional[float] = None,
    ):
        """
        Initialize constant rate executor.

        Args:
            func: Async function to execute
            rate_hz: Target execution rate in Hz (None for as-fast-as-possible)
        """
        self.func = func
        self._running = False
        self._rate_hz = rate_hz

        if rate_hz is None:
            self._run_impl = self._async_run
        else:
            self.interval = 1.0 / rate_hz
            self._run_impl = self._constant_rate_run

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

    async def _async_run(self, *args, **kwargs) -> None:
        """Run the async function as fast as possible."""
        while self._running:
            try:
                await self.func(*args, **kwargs)
                await asyncio.sleep(0)  # Yield to event loop
            except asyncio.CancelledError:
                break
            except Exception as e:
                logger.error(f"Error in _async_run: {e}")
                await asyncio.sleep(0.001)

    async def _constant_rate_run(self, *args, **kwargs) -> None:
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
                logger.error(f"Error in _constant_rate_run: {e}")
                await asyncio.sleep(0.001)

            # Sleep until next execution time (with small max to stay responsive)
            sleep_time = min(
                max(0, next_exec_time - time.perf_counter()),
                0.001,  # Max sleep to stay responsive
            )
            await asyncio.sleep(sleep_time)


class MultiRateExecutor:
    """
    Execute multiple async functions concurrently at different rates.

    Each function can have its own execution rate.

    Example:
        executor = MultiRateExecutor()
        executor.add(sensor_read, rate_hz=100.0)
        executor.add(process_data, rate_hz=30.0)
        executor.add(publish_results, rate_hz=10.0)

        await executor.run()
    """

    def __init__(self):
        """Initialize multi-rate executor."""
        self._executors: list[RateExecutor] = []
        self._tasks: list[asyncio.Task] = []
        self._running = False

    def add(
        self,
        func: AsyncCallback,
        rate_hz: Optional[float] = None,
    ) -> RateExecutor:
        """
        Add a function to execute.

        Args:
            func: Async function to execute
            rate_hz: Target rate in Hz (None for as-fast-as-possible)

        Returns:
            The created RateExecutor
        """
        executor = RateExecutor(func=func, rate_hz=rate_hz)
        self._executors.append(executor)
        return executor

    @property
    def running(self) -> bool:
        """Check if any executor is running."""
        return self._running

    def start(self) -> None:
        """Start all executors."""
        self._running = True
        for executor in self._executors:
            executor.start()

    def stop(self) -> None:
        """Stop all executors."""
        self._running = False
        for executor in self._executors:
            executor.stop()

    async def run(self) -> None:
        """Run all executors concurrently."""
        self.start()

        try:
            self._tasks = [
                asyncio.create_task(executor.run()) for executor in self._executors
            ]

            await asyncio.gather(*self._tasks, return_exceptions=True)
        except asyncio.CancelledError:
            pass
        finally:
            self.stop()
            for task in self._tasks:
                if not task.done():
                    task.cancel()
            await asyncio.gather(*self._tasks, return_exceptions=True)
            self._tasks.clear()
