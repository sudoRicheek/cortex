"""
Hashing utilities for computing 64-bit fingerprints.

The fingerprint is used to identify message types for fast decoding
without needing to parse the entire message structure.
"""

import hashlib
import struct
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from cortex.messages.base import Message


def compute_fingerprint(message_class: type["Message"]) -> int:
    """
    Compute a 64-bit fingerprint for a message class.

    The fingerprint is based on the fully qualified class name and
    the field names/types to ensure type safety across processes.

    Args:
        message_class: The message class to compute fingerprint for.

    Returns:
        A 64-bit unsigned integer fingerprint.
    """
    # Build a canonical string representation of the message type
    class_name = f"{message_class.__module__}.{message_class.__qualname__}"

    # Include field information for structural fingerprinting
    field_info = []
    if hasattr(message_class, "__dataclass_fields__"):
        for name, field in message_class.__dataclass_fields__.items():
            field_type = getattr(field.type, "__name__", str(field.type))
            field_info.append(f"{name}:{field_type}")

    canonical = f"{class_name}|{','.join(sorted(field_info))}"

    # Compute SHA-256 and take first 8 bytes as 64-bit fingerprint
    hash_bytes = hashlib.sha256(canonical.encode("utf-8")).digest()
    fingerprint = struct.unpack(">Q", hash_bytes[:8])[0]

    return fingerprint


def fingerprint_to_bytes(fingerprint: int) -> bytes:
    """Convert a 64-bit fingerprint to bytes (big-endian)."""
    return struct.pack(">Q", fingerprint)


def bytes_to_fingerprint(data: bytes) -> int:
    """Convert bytes to a 64-bit fingerprint (big-endian)."""
    return struct.unpack(">Q", data[:8])[0]


# Cache for fingerprints to avoid recomputation
_fingerprint_cache: dict[type["Message"], int] = {}


def get_cached_fingerprint(message_class: type["Message"]) -> int:
    """Get or compute fingerprint with caching."""
    if message_class not in _fingerprint_cache:
        _fingerprint_cache[message_class] = compute_fingerprint(message_class)
    return _fingerprint_cache[message_class]


def register_fingerprint(message_class: type["Message"], fingerprint: int) -> None:
    """Manually register a fingerprint for a message class."""
    _fingerprint_cache[message_class] = fingerprint


def clear_fingerprint_cache() -> None:
    """Clear the fingerprint cache (useful for testing)."""
    _fingerprint_cache.clear()
