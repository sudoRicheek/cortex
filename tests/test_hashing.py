"""
Tests for the hashing utilities.
"""

from dataclasses import dataclass

from cortex.messages.base import Message
from cortex.utils.hashing import (
    bytes_to_fingerprint,
    clear_fingerprint_cache,
    compute_fingerprint,
    fingerprint_to_bytes,
    get_cached_fingerprint,
)


class TestFingerprint:
    """Tests for fingerprint computation."""

    def setup_method(self):
        """Clear cache before each test."""
        clear_fingerprint_cache()

    def test_fingerprint_is_64_bit(self):
        """Fingerprint should be a 64-bit integer."""

        @dataclass
        class TestMsg(Message):
            value: int

        fp = compute_fingerprint(TestMsg)
        assert isinstance(fp, int)
        assert 0 <= fp < 2**64

    def test_fingerprint_consistency(self):
        """Same class should always produce same fingerprint."""

        @dataclass
        class TestMsg(Message):
            value: int

        fp1 = compute_fingerprint(TestMsg)
        fp2 = compute_fingerprint(TestMsg)
        assert fp1 == fp2

    def test_different_classes_different_fingerprints(self):
        """Different classes should have different fingerprints."""

        @dataclass
        class MsgA(Message):
            value: int

        @dataclass
        class MsgB(Message):
            value: int

        fp_a = compute_fingerprint(MsgA)
        fp_b = compute_fingerprint(MsgB)
        assert fp_a != fp_b

    def test_different_fields_different_fingerprints(self):
        """Classes with different fields should have different fingerprints."""

        @dataclass
        class MsgInt(Message):
            value: int

        @dataclass
        class MsgStr(Message):
            value: str

        fp_int = compute_fingerprint(MsgInt)
        fp_str = compute_fingerprint(MsgStr)
        assert fp_int != fp_str

    def test_fingerprint_to_bytes_roundtrip(self):
        """Fingerprint should survive bytes conversion."""
        original = 0x123456789ABCDEF0

        as_bytes = fingerprint_to_bytes(original)
        assert len(as_bytes) == 8

        restored = bytes_to_fingerprint(as_bytes)
        assert restored == original

    def test_cached_fingerprint(self):
        """Cached fingerprint should return same value."""

        @dataclass
        class CachedMsg(Message):
            data: str

        fp1 = get_cached_fingerprint(CachedMsg)
        fp2 = get_cached_fingerprint(CachedMsg)
        assert fp1 == fp2

    def test_clear_cache(self):
        """Cache clearing should work."""
        from cortex.messages.standard import StringMessage

        # Use a stable message class (not defined inline)
        fp1 = get_cached_fingerprint(StringMessage)
        clear_fingerprint_cache()
        fp2 = get_cached_fingerprint(StringMessage)

        # Should be same value but computed fresh
        assert fp1 == fp2
