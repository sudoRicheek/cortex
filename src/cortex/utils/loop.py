"""
Event loop utilities for Cortex.

Provides uvloop integration for improved async performance on Unix systems.
"""

import asyncio
import importlib.util
import logging
import sys

logger = logging.getLogger("cortex.loop")

# Try to use uvloop on Unix systems
_uvloop_available = (
    sys.platform != "win32" and importlib.util.find_spec("uvloop") is not None
)


def run(coro, *, debug: bool = False):
    """
    Run a coroutine using uvloop if available.

    This is a replacement for asyncio.run() that uses uvloop on Unix.

    Args:
        coro: Coroutine to run
        debug: Enable debug mode

    Returns:
        The result of the coroutine
    """
    if _uvloop_available:
        import uvloop

        logger.info("Using uvloop event loop")
        return uvloop.run(coro, debug=debug)

    logger.info("Using asyncio event loop")
    return asyncio.run(coro, debug=debug)
