"""
Base message classes for Cortex.

All message types should inherit from Message and use the @dataclass decorator.
The message system provides automatic serialization and 64-bit fingerprinting.
"""

import struct
import time
from dataclasses import dataclass, fields
from typing import ClassVar, TypeVar

from cortex.utils.hashing import (
    get_cached_fingerprint,
)
from cortex.utils.serialization import deserialize_message_data, serialize_message_data

T = TypeVar("T", bound="Message")


class MessageType:
    """
    Registry for message types by fingerprint.

    This allows automatic deserialization based on the 64-bit fingerprint
    sent with each message.
    """

    _registry: ClassVar[dict[int, type["Message"]]] = {}

    @classmethod
    def register(cls, message_class: type["Message"]) -> type["Message"]:
        """Register a message class by its fingerprint."""
        fingerprint = get_cached_fingerprint(message_class)
        cls._registry[fingerprint] = message_class
        return message_class

    @classmethod
    def get(cls, fingerprint: int) -> type["Message"] | None:
        """Get a message class by fingerprint."""
        return cls._registry.get(fingerprint)

    @classmethod
    def get_all(cls) -> dict[int, type["Message"]]:
        """Get all registered message types."""
        return cls._registry.copy()

    @classmethod
    def clear(cls) -> None:
        """Clear the registry (useful for testing)."""
        cls._registry.clear()


@dataclass
class MessageHeader:
    """
    Header for all Cortex messages.

    Contains metadata that is sent with every message.
    """

    fingerprint: int  # 64-bit type identifier
    timestamp_ns: int  # Nanosecond timestamp
    sequence: int  # Sequence number

    def to_bytes(self) -> bytes:
        """Serialize header to bytes (24 bytes fixed size)."""
        return struct.pack(">QQQ", self.fingerprint, self.timestamp_ns, self.sequence)

    @classmethod
    def from_bytes(cls, data: bytes) -> "MessageHeader":
        """Deserialize header from bytes."""
        fingerprint, timestamp_ns, sequence = struct.unpack(">QQQ", data[:24])
        return cls(
            fingerprint=fingerprint, timestamp_ns=timestamp_ns, sequence=sequence
        )

    @classmethod
    def size(cls) -> int:
        """Return the fixed header size in bytes."""
        return 24


@dataclass
class Message:
    """
    Base class for all Cortex messages.

    Subclasses should be decorated with @dataclass and define their
    fields. The message will automatically compute its fingerprint
    based on the class name and field structure.

    Example:
        @dataclass
        class PointCloud(Message):
            points: np.ndarray
            colors: np.ndarray
            intensity: float = 1.0
    """

    # Class-level sequence counter
    _sequence_counter: ClassVar[int] = 0

    def __init_subclass__(cls, **kwargs):
        """Automatically register subclasses."""
        super().__init_subclass__(**kwargs)
        # Only register concrete classes (not abstract ones)
        if not getattr(cls, "__abstractmethods__", None):
            MessageType.register(cls)

    @classmethod
    def fingerprint(cls) -> int:
        """Get the 64-bit fingerprint for this message type."""
        return get_cached_fingerprint(cls)

    @classmethod
    def _next_sequence(cls) -> int:
        """Get the next sequence number."""
        seq = cls._sequence_counter
        cls._sequence_counter += 1
        return seq

    def to_bytes(self) -> bytes:
        """
        Serialize the message to bytes.

        Format:
        - 24 bytes: header (fingerprint, timestamp, sequence)
        - remaining: serialized field data
        """
        # Build header
        header = MessageHeader(
            fingerprint=self.fingerprint(),
            timestamp_ns=time.time_ns(),
            sequence=self._next_sequence(),
        )

        # Get field data (excluding inherited fields from dataclass machinery)
        field_data = {}
        for f in fields(self):
            field_data[f.name] = getattr(self, f.name)

        # Serialize
        header_bytes = header.to_bytes()
        data_bytes = serialize_message_data(field_data)

        return header_bytes + data_bytes

    @classmethod
    def from_bytes(cls: type[T], data: bytes) -> tuple[T, MessageHeader]:
        """
        Deserialize a message from bytes.

        Returns:
            Tuple of (message instance, header)
        """
        # Parse header
        header = MessageHeader.from_bytes(data)

        # Parse field data
        field_data = deserialize_message_data(data[MessageHeader.size() :])

        # Create instance
        instance = cls(**field_data)

        return instance, header

    @staticmethod
    def decode(data: bytes) -> tuple["Message", MessageHeader]:
        """
        Decode a message without knowing its type in advance.

        Uses the fingerprint in the header to look up the message type.

        Returns:
            Tuple of (message instance, header)

        Raises:
            ValueError: If the message type is not registered
        """
        header = MessageHeader.from_bytes(data)
        message_class = MessageType.get(header.fingerprint)

        if message_class is None:
            raise ValueError(
                f"Unknown message type with fingerprint: {header.fingerprint:#018x}"
            )

        return message_class.from_bytes(data)
