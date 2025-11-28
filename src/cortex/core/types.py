"""
Type aliases for Cortex core module.

This module contains all type aliases used across the core module
to ensure consistency and avoid circular imports.
"""

from collections.abc import Callable, Coroutine
from typing import Any

from cortex.messages.base import Message, MessageHeader

# Async callback with any arguments
AsyncCallback = Callable[..., Coroutine[Any, Any, None]]

# Callback for message reception: (message, header) -> None
MessageCallback = Callable[[Message, MessageHeader], Coroutine[Any, Any, None]]
