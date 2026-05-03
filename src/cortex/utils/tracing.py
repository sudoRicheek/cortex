"""
Per-stage latency tracing for the Cortex receive path.

Tracing is **off by default**: when ``CORTEX_TRACE_LATENCY`` is unset (or
``0``), :func:`stage` is a near-zero-cost context manager that does not
allocate. When the env var is set to a positive integer, that many of the
most recent samples per stage are kept in a ring buffer that benchmarks
and tests can read out via :func:`snapshot`.

This is intentionally tiny — the whole point of measuring sub-100 µs
paths is that the instrumentation itself can't be on the order of the
thing being measured. We use ``time.perf_counter_ns`` for monotonic
nanoseconds and avoid any logging or string formatting in the hot path.
"""

import os
import threading
from collections import deque
from collections.abc import Iterator
from contextlib import contextmanager
from time import perf_counter_ns


def _trace_capacity() -> int:
    raw = os.environ.get("CORTEX_TRACE_LATENCY", "0")
    try:
        n = int(raw)
    except ValueError:
        return 0
    return max(n, 0)


_CAPACITY = _trace_capacity()
_ENABLED = _CAPACITY > 0

_lock = threading.Lock()
_samples: dict[str, deque[int]] = {}


def enabled() -> bool:
    """Return True when latency tracing is active for this process."""
    return _ENABLED


@contextmanager
def stage(name: str) -> Iterator[None]:
    """Time a code region and record the elapsed nanoseconds under ``name``.

    No-op (and zero allocations beyond the generator object) when tracing
    is disabled.
    """
    if not _ENABLED:
        yield
        return
    start = perf_counter_ns()
    try:
        yield
    finally:
        elapsed = perf_counter_ns() - start
        with _lock:
            buf = _samples.get(name)
            if buf is None:
                buf = deque(maxlen=_CAPACITY)
                _samples[name] = buf
            buf.append(elapsed)


def record(name: str, nanoseconds: int) -> None:
    """Record a pre-computed elapsed time for ``name`` (no-op when off)."""
    if not _ENABLED:
        return
    with _lock:
        buf = _samples.get(name)
        if buf is None:
            buf = deque(maxlen=_CAPACITY)
            _samples[name] = buf
        buf.append(int(nanoseconds))


def snapshot() -> dict[str, list[int]]:
    """Return a copy of all collected samples in nanoseconds."""
    with _lock:
        return {k: list(v) for k, v in _samples.items()}


def reset() -> None:
    """Clear all recorded samples."""
    with _lock:
        _samples.clear()
