"""
Runtime introspection helpers for Cortex.

The synchronous subscriber path relies on dedicated OS threads. Its
latency floor is dramatically tighter on a free-threaded build of CPython
(PEP 779, available as ``python3.14t``) because the receive thread does
not contend with the asyncio thread for the GIL.

These helpers let the rest of the framework probe the running interpreter
once and adapt — defaults, log lines, and capability checks live here so
hot paths don't re-discover the environment per call.
"""

import os
import platform
import sys
import sysconfig
from dataclasses import dataclass
from functools import cache


@dataclass(frozen=True, slots=True)
class RuntimeInfo:
    """Snapshot of the Python runtime relevant to Cortex tuning."""

    python_version: tuple[int, int, int]
    implementation: str
    free_threaded: bool
    """True when the GIL is disabled at runtime (PEP 779)."""
    free_threaded_build: bool
    """True when running on a ``python3.14t``-style build that *can* disable
    the GIL — even if a C extension has re-enabled it at runtime."""
    gil_supported: bool
    """True when the interpreter exposes ``sys._is_gil_enabled`` (CPython 3.13+)."""
    cpu_count: int
    """Number of CPUs available to this process (``os.process_cpu_count`` on 3.13+)."""


@cache
def runtime_info() -> RuntimeInfo:
    """Return an immutable snapshot of the running interpreter."""
    version = sys.version_info[:3]
    implementation = platform.python_implementation()

    gil_probe = getattr(sys, "_is_gil_enabled", None)
    gil_supported = gil_probe is not None
    free_threaded = gil_supported and not gil_probe()  # type: ignore[misc]
    free_threaded_build = bool(sysconfig.get_config_var("Py_GIL_DISABLED"))

    process_cpu_count = getattr(os, "process_cpu_count", None)
    if process_cpu_count is not None:
        cpu_count = process_cpu_count() or os.cpu_count() or 1
    else:
        cpu_count = os.cpu_count() or 1

    return RuntimeInfo(
        python_version=version,
        implementation=implementation,
        free_threaded=free_threaded,
        free_threaded_build=free_threaded_build,
        gil_supported=gil_supported,
        cpu_count=cpu_count,
    )


def is_free_threaded() -> bool:
    """Convenience accessor: ``True`` on free-threaded CPython."""
    return runtime_info().free_threaded


def low_latency_advisory() -> str | None:
    """Return a one-line hint when a control-loop subscriber would benefit
    from a free-threaded interpreter.

    Returns ``None`` if the current runtime is already optimal, otherwise a
    short suggestion string suitable for a single ``logger.info`` call at
    subscriber start.
    """
    info = runtime_info()
    if info.free_threaded:
        return None
    if info.free_threaded_build:
        # python3.14t binary, but a C extension (commonly msgpack) re-enabled
        # the GIL on import. The workaround is documented and safe enough
        # for benchmarks.
        return (
            "Sync subscriber running on a free-threaded Python build with "
            "the GIL re-enabled (likely a C extension that has not declared "
            "free-thread safety). Set PYTHON_GIL=0 to override and unlock "
            "free-threaded behavior."
        )
    if info.python_version < (3, 14):
        return (
            "Sync subscriber running on Python "
            f"{info.python_version[0]}.{info.python_version[1]} with GIL enabled; "
            "for tighter tail latency use python3.14t (free-threaded build)."
        )
    return (
        "Sync subscriber running on Python 3.14 with GIL enabled; "
        "switch to python3.14t (free-threaded) for tighter tail latency."
    )
