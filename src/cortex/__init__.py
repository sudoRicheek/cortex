"""
Cortex: A lightweight framework using ZeroMQ for IPC.

This framework provides:
- Publisher/Subscriber pattern for inter-process communication
- Discovery service for automatic topic resolution
- Support for numpy arrays, torch tensors, and Python dicts
- 64-bit fingerprint hashing for message type identification
- Asyncio-based architecture for cooperative multitasking (with uvloop on Unix)

The top-level package exposes only the core API: ``Node``, ``Publisher``,
``Subscriber``, ``run``, ``Message``, and ``MessageType``. Standard message
implementations live in :mod:`cortex.messages.standard`; executors in
:mod:`cortex.core.executor`; discovery in :mod:`cortex.discovery`.
"""

from cortex.core.node import Node
from cortex.core.publisher import Publisher
from cortex.core.subscriber import Subscriber
from cortex.messages.base import Message, MessageType
from cortex.utils.loop import run

__version__ = "0.1.0"
__all__ = [
    "Node",
    "Publisher",
    "Subscriber",
    "Message",
    "MessageType",
    "run",
]
