"""Base message classes for Cortex."""

import struct
import time
from dataclasses import dataclass, fields
from typing import ClassVar, TypeVar

from cortex.utils.hashing import (
    get_cached_fingerprint,
)
from cortex.utils.serialization import (
    deserialize_message_frames,
    deserialize_message_values,
    serialize_message_frames,
    serialize_message_values,
)

T = TypeVar("T", bound="Message")


def _frame_to_bytes_like(frame: object) -> bytes | memoryview:
    """Convert transport frame objects into bytes-like buffers."""
    if hasattr(frame, "buffer"):
        return memoryview(frame.buffer)
    return frame


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
    _field_names_cache: ClassVar[tuple[str, ...] | None] = None

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

    @classmethod
    def _field_names(cls) -> tuple[str, ...]:
        """Get cached dataclass field names in declaration order."""
        cached = cls.__dict__.get("_field_names_cache")
        if cached is None:
            cached = tuple(field.name for field in fields(cls))
            cls._field_names_cache = cached
        return cached

    def _field_values(self) -> list[object]:
        """Get field values in schema order."""
        return [getattr(self, name) for name in self._field_names()]

    @classmethod
    def _build_instance(cls: type[T], values: list[object]) -> T:
        """Create a message instance from ordered field values."""
        field_names = cls._field_names()
        if len(values) != len(field_names):
            raise ValueError(
                f"Expected {len(field_names)} fields for {cls.__name__}, got {len(values)}"
            )
        return cls(**dict(zip(field_names, values, strict=True)))

    def _build_header(self) -> MessageHeader:
        """Create a message header for the current instance."""
        return MessageHeader(
            fingerprint=self.fingerprint(),
            timestamp_ns=time.time_ns(),
            sequence=self._next_sequence(),
        )

    def to_bytes(self) -> bytes:
        """
        Serialize the message to bytes.

        Format:
        - 24 bytes: header (fingerprint, timestamp, sequence)
        - remaining: serialized field data
        """
        header_bytes = self._build_header().to_bytes()
        data_bytes = serialize_message_values(self._field_values())
        return header_bytes + data_bytes

    def to_frames(self) -> list[object]:
        """Serialize the message into transport frames.

        The first frame is always the fixed-size header. The second frame holds
        packed metadata, and any remaining frames are raw out-of-band buffers.
        """
        return [
            self._build_header().to_bytes(),
            *serialize_message_frames(self._field_values()),
        ]

    @classmethod
    def from_bytes(cls: type[T], data: bytes) -> tuple[T, MessageHeader]:
        """
        Deserialize a message from bytes.

        Returns:
            Tuple of (message instance, header)
        """
        header = MessageHeader.from_bytes(data)
        values = deserialize_message_values(data[MessageHeader.size() :])
        return cls._build_instance(values), header

    @classmethod
    def from_frames(cls: type[T], frames: list[object]) -> tuple[T, MessageHeader]:
        """Deserialize a message from transport frames."""
        if len(frames) < 2:
            raise ValueError("Message frame payload must include header and metadata")

        header = MessageHeader.from_bytes(_frame_to_bytes_like(frames[0]))
        values = deserialize_message_frames(frames[1:])
        return cls._build_instance(values), header

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
