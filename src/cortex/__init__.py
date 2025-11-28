"""
Cortex: A lightweight framework using ZeroMQ for IPC.

This framework provides:
- Publisher/Subscriber pattern for inter-process communication
- Discovery service for automatic topic resolution
- Support for numpy arrays, torch tensors, and Python dicts
- 64-bit fingerprint hashing for message type identification
- Asyncio-based architecture for cooperative multitasking
"""

from cortex.core.executor import MultiRateExecutor, RateExecutor
from cortex.core.node import Node
from cortex.core.publisher import Publisher
from cortex.core.subscriber import Subscriber
from cortex.messages.base import Message, MessageType
from cortex.messages.standard import (
    ArrayMessage,
    DictMessage,
    FloatMessage,
    IntMessage,
    StringMessage,
    TensorMessage,
)

__version__ = "0.1.0"
__all__ = [
    "Node",
    "Publisher",
    "Subscriber",
    "RateExecutor",
    "MultiRateExecutor",
    "Message",
    "MessageType",
    "ArrayMessage",
    "TensorMessage",
    "DictMessage",
    "StringMessage",
    "FloatMessage",
    "IntMessage",
]
