"""
Event loop utilities for Cortex.

Provides uvloop integration for improved async performance on Unix systems.
"""

import asyncio
import importlib.util
import logging
import sys
from collections.abc import Coroutine
from typing import Any

logger = logging.getLogger("cortex.loop")

# Try to use uvloop on Unix systems
_uvloop_available = (
    sys.platform != "win32" and importlib.util.find_spec("uvloop") is not None
)


def run(coro: Coroutine[Any, Any, Any], *, debug: bool = False) -> Any:
    """Run a coroutine, preferring ``uvloop`` when available.

    Drop-in replacement for :func:`asyncio.run`. On Unix with ``uvloop``
    installed, this yields noticeably lower tail latency on high-rate
    small-message workloads.

    Args:
        coro: The top-level coroutine to run to completion.
        debug: Pass through to the event loop's ``debug`` flag.

    Returns:
        Whatever ``coro`` returns.
    """
    if _uvloop_available:
        import uvloop

        logger.info("Using uvloop event loop")
        return uvloop.run(coro, debug=debug)

    logger.info("Using asyncio event loop")
    return asyncio.run(coro, debug=debug)
