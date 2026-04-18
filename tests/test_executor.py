"""
Tests for executor classes.
"""

import asyncio
import contextlib
import time

import pytest

from cortex.core.executor import AsyncExecutor, BaseExecutor, RateExecutor


class TestBaseExecutor:
    """Tests for BaseExecutor base class."""

    def test_cannot_instantiate_base_executor(self):
        """BaseExecutor is abstract and cannot be instantiated."""

        async def dummy():
            pass

        with pytest.raises(TypeError):
            BaseExecutor(dummy)

    def test_start_sets_running(self):
        """Start should set running to True."""

        async def dummy():
            pass

        executor = AsyncExecutor(dummy)
        assert not executor.running

        executor.start()
        assert executor.running

    def test_stop_clears_running(self):
        """Stop should set running to False."""

        async def dummy():
            pass

        executor = AsyncExecutor(dummy)
        executor.start()
        assert executor.running

        executor.stop()
        assert not executor.running


class TestAsyncExecutor:
    """Tests for AsyncExecutor class."""

    @pytest.mark.asyncio
    async def test_executes_callback(self):
        """AsyncExecutor should execute the callback."""
        call_count = 0

        async def callback():
            nonlocal call_count
            call_count += 1

        executor = AsyncExecutor(callback)

        # Run for a short time
        run_task = asyncio.create_task(executor.run())
        await asyncio.sleep(0.05)
        executor.stop()

        with contextlib.suppress(asyncio.CancelledError):
            await run_task

        assert call_count > 0

    @pytest.mark.asyncio
    async def test_runs_as_fast_as_possible(self):
        """AsyncExecutor should run many times quickly."""
        call_count = 0

        async def callback():
            nonlocal call_count
            call_count += 1

        executor = AsyncExecutor(callback)

        run_task = asyncio.create_task(executor.run())
        await asyncio.sleep(0.1)
        executor.stop()

        with contextlib.suppress(asyncio.CancelledError):
            await run_task

        # Should execute many times in 100ms
        assert call_count > 100

    @pytest.mark.asyncio
    async def test_run_calls_start_and_stop(self):
        """Run should call start() and stop() automatically."""
        states: list[bool] = []

        async def callback():
            # Capture running state during execution
            pass

        executor = AsyncExecutor(callback)

        # Before run
        assert not executor.running

        run_task = asyncio.create_task(executor.run())
        await asyncio.sleep(0.01)

        # During run
        states.append(executor.running)

        executor.stop()
        with contextlib.suppress(asyncio.CancelledError):
            await run_task

        # After run completes, should still be stopped
        assert not executor.running
        assert states[0] is True  # Was running during execution

    @pytest.mark.asyncio
    async def test_handles_callback_exception(self):
        """AsyncExecutor should handle exceptions in callback."""
        call_count = 0

        async def failing_callback():
            nonlocal call_count
            call_count += 1
            if call_count < 3:
                raise ValueError("Test error")

        executor = AsyncExecutor(failing_callback)

        run_task = asyncio.create_task(executor.run())
        await asyncio.sleep(0.05)
        executor.stop()

        with contextlib.suppress(asyncio.CancelledError):
            await run_task

        # Should continue despite exceptions
        assert call_count >= 3

    @pytest.mark.asyncio
    async def test_stops_on_cancellation(self):
        """AsyncExecutor should stop cleanly on task cancellation."""
        call_count = 0

        async def callback():
            nonlocal call_count
            call_count += 1
            await asyncio.sleep(0.01)

        executor = AsyncExecutor(callback)
        run_task = asyncio.create_task(executor.run())

        await asyncio.sleep(0.05)
        run_task.cancel()

        with contextlib.suppress(asyncio.CancelledError):
            await run_task

        assert not executor.running


class TestRateExecutor:
    """Tests for RateExecutor class."""

    @pytest.mark.asyncio
    async def test_executes_at_target_rate(self):
        """RateExecutor should execute approximately at target rate."""
        call_times: list[float] = []

        async def callback():
            call_times.append(time.perf_counter())

        rate_hz = 20.0  # 20 Hz = 50ms interval
        executor = RateExecutor(callback, rate_hz=rate_hz)

        run_task = asyncio.create_task(executor.run())
        await asyncio.sleep(0.5)  # Run for 500ms
        executor.stop()

        with contextlib.suppress(asyncio.CancelledError):
            await run_task

        # Should have approximately 10 calls in 500ms at 20Hz
        assert 8 <= len(call_times) <= 15

    @pytest.mark.asyncio
    async def test_interval_property(self):
        """RateExecutor should have correct interval."""

        async def callback():
            pass

        executor = RateExecutor(callback, rate_hz=10.0)
        assert executor.interval == 0.1  # 100ms

        executor2 = RateExecutor(callback, rate_hz=100.0)
        assert executor2.interval == 0.01  # 10ms

    @pytest.mark.asyncio
    async def test_run_calls_start_and_stop(self):
        """Run should call start() and stop() automatically."""

        async def callback():
            pass

        executor = RateExecutor(callback, rate_hz=10.0)

        assert not executor.running

        run_task = asyncio.create_task(executor.run())
        await asyncio.sleep(0.05)

        assert executor.running

        executor.stop()
        with contextlib.suppress(asyncio.CancelledError):
            await run_task

        assert not executor.running

    @pytest.mark.asyncio
    async def test_handles_callback_exception(self):
        """RateExecutor should handle exceptions in callback."""
        call_count = 0

        async def failing_callback():
            nonlocal call_count
            call_count += 1
            if call_count < 3:
                raise ValueError("Test error")

        executor = RateExecutor(failing_callback, rate_hz=100.0)

        run_task = asyncio.create_task(executor.run())
        await asyncio.sleep(0.1)
        executor.stop()

        with contextlib.suppress(asyncio.CancelledError):
            await run_task

        # Should continue despite exceptions
        assert call_count >= 3

    @pytest.mark.asyncio
    async def test_maintains_timing_with_slow_callback(self):
        """RateExecutor should maintain timing even with slow callbacks."""
        call_times: list[float] = []

        async def slow_callback():
            call_times.append(time.perf_counter())
            await asyncio.sleep(0.02)  # 20ms work

        # 10 Hz = 100ms interval, callback takes 20ms
        executor = RateExecutor(slow_callback, rate_hz=10.0)

        run_task = asyncio.create_task(executor.run())
        await asyncio.sleep(0.5)
        executor.stop()

        with contextlib.suppress(asyncio.CancelledError):
            await run_task

        # Should still execute approximately on schedule
        assert 4 <= len(call_times) <= 7

    @pytest.mark.asyncio
    async def test_stops_on_cancellation(self):
        """RateExecutor should stop cleanly on task cancellation."""
        call_count = 0

        async def callback():
            nonlocal call_count
            call_count += 1

        executor = RateExecutor(callback, rate_hz=10.0)
        run_task = asyncio.create_task(executor.run())

        await asyncio.sleep(0.15)
        run_task.cancel()

        with contextlib.suppress(asyncio.CancelledError):
            await run_task

        assert not executor.running


class TestExecutorIntegration:
    """Integration tests for executors."""

    @pytest.mark.asyncio
    async def test_multiple_executors_concurrent(self):
        """Multiple executors should run concurrently."""
        async_count = 0
        rate_count = 0

        async def async_callback():
            nonlocal async_count
            async_count += 1

        async def rate_callback():
            nonlocal rate_count
            rate_count += 1

        async_exec = AsyncExecutor(async_callback)
        rate_exec = RateExecutor(rate_callback, rate_hz=50.0)

        task1 = asyncio.create_task(async_exec.run())
        task2 = asyncio.create_task(rate_exec.run())

        await asyncio.sleep(0.2)

        async_exec.stop()
        rate_exec.stop()

        with contextlib.suppress(asyncio.CancelledError):
            await asyncio.gather(task1, task2, return_exceptions=True)

        # Both should have executed
        assert async_count > 100  # AsyncExecutor runs fast
        assert 8 <= rate_count <= 15  # ~10 at 50Hz in 200ms

    @pytest.mark.asyncio
    async def test_executor_with_args(self):
        """Executors should pass args to callback."""
        received_args: list[tuple] = []

        async def callback(*args, **kwargs):
            received_args.append((args, kwargs))

        executor = AsyncExecutor(callback)

        run_task = asyncio.create_task(executor.run(1, 2, key="value"))
        await asyncio.sleep(0.02)
        executor.stop()

        with contextlib.suppress(asyncio.CancelledError):
            await run_task

        assert len(received_args) > 0
        assert received_args[0] == ((1, 2), {"key": "value"})
