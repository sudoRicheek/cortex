"""
Tests for the message system.
"""

from dataclasses import dataclass

import numpy as np

from cortex.messages.base import Message, MessageHeader, MessageType
from cortex.messages.standard import (
    ArrayMessage,
    DictMessage,
    FloatMessage,
    ImageMessage,
    IntMessage,
    PointCloudMessage,
    StringMessage,
)
from cortex.utils.hashing import clear_fingerprint_cache


class TestMessageBase:
    """Tests for base Message class."""

    def setup_method(self):
        """Clear registries before each test."""
        clear_fingerprint_cache()
        MessageType.clear()

    def test_message_fingerprint(self):
        """Messages should have a fingerprint."""

        @dataclass
        class TestMsg(Message):
            value: int

        fp = TestMsg.fingerprint()
        assert isinstance(fp, int)
        assert fp > 0

    def test_message_auto_registration(self):
        """Messages should auto-register with MessageType."""

        @dataclass
        class AutoRegMsg(Message):
            data: str

        fp = AutoRegMsg.fingerprint()
        registered = MessageType.get(fp)
        assert registered is AutoRegMsg

    def test_message_to_bytes(self):
        """Messages should serialize to bytes."""

        @dataclass
        class BytesMsg(Message):
            value: int
            name: str

        msg = BytesMsg(value=42, name="test")
        data = msg.to_bytes()

        assert isinstance(data, bytes)
        assert len(data) > MessageHeader.size()

    def test_message_from_bytes(self):
        """Messages should deserialize from bytes."""

        @dataclass
        class RoundtripMsg(Message):
            value: int
            name: str

        original = RoundtripMsg(value=42, name="test")
        data = original.to_bytes()

        restored, header = RoundtripMsg.from_bytes(data)

        assert restored.value == original.value
        assert restored.name == original.name
        assert header.fingerprint == RoundtripMsg.fingerprint()

    def test_message_frame_roundtrip(self):
        """Messages should roundtrip through the frame transport path."""

        @dataclass
        class FrameMsg(Message):
            value: int
            data: np.ndarray

        original = FrameMsg(value=7, data=np.arange(12, dtype=np.float32).reshape(3, 4))
        frames = original.to_frames()

        restored, header = FrameMsg.from_frames(frames)

        assert restored.value == original.value
        np.testing.assert_array_equal(restored.data, original.data)
        assert header.fingerprint == FrameMsg.fingerprint()

    def test_message_decode(self):
        """Messages should decode without knowing type."""

        @dataclass
        class DecodeMsg(Message):
            data: int

        original = DecodeMsg(data=123)
        data = original.to_bytes()

        restored, header = Message.decode(data)

        assert isinstance(restored, DecodeMsg)
        assert restored.data == 123

    def test_message_header_timestamp(self):
        """Message header should have valid timestamp."""

        @dataclass
        class TimestampMsg(Message):
            x: int

        msg = TimestampMsg(x=1)
        data = msg.to_bytes()

        _, header = TimestampMsg.from_bytes(data)

        assert header.timestamp_ns > 0

    def test_message_header_sequence(self):
        """Message headers should have incrementing sequences."""

        @dataclass
        class SeqMsg(Message):
            x: int

        msg1 = SeqMsg(x=1)
        msg2 = SeqMsg(x=2)

        data1 = msg1.to_bytes()
        data2 = msg2.to_bytes()

        _, header1 = SeqMsg.from_bytes(data1)
        _, header2 = SeqMsg.from_bytes(data2)

        assert header2.sequence > header1.sequence


class TestStandardMessages:
    """Tests for standard message types."""

    def test_string_message(self):
        """StringMessage should work."""
        msg = StringMessage(data="hello world")
        data = msg.to_bytes()
        restored, _ = StringMessage.from_bytes(data)

        assert restored.data == "hello world"

    def test_int_message(self):
        """IntMessage should work."""
        msg = IntMessage(data=42)
        data = msg.to_bytes()
        restored, _ = IntMessage.from_bytes(data)

        assert restored.data == 42

    def test_float_message(self):
        """FloatMessage should work."""
        msg = FloatMessage(data=3.14159)
        data = msg.to_bytes()
        restored, _ = FloatMessage.from_bytes(data)

        assert abs(restored.data - 3.14159) < 1e-5

    def test_dict_message(self):
        """DictMessage should work."""
        msg = DictMessage(
            data={"key1": "value1", "key2": 42, "nested": {"inner": True}}
        )
        data = msg.to_bytes()
        restored, _ = DictMessage.from_bytes(data)

        assert restored.data["key1"] == "value1"
        assert restored.data["key2"] == 42
        assert restored.data["nested"]["inner"]

    def test_array_message(self):
        """ArrayMessage should work."""
        arr = np.random.randn(100, 100).astype(np.float32)
        msg = ArrayMessage(data=arr, name="test_array", frame_id="world")

        data = msg.to_bytes()
        restored, _ = ArrayMessage.from_bytes(data)

        np.testing.assert_array_almost_equal(arr, restored.data)
        assert restored.name == "test_array"
        assert restored.frame_id == "world"

    def test_image_message(self):
        """ImageMessage should work."""
        img = np.random.randint(0, 256, (480, 640, 3), dtype=np.uint8)
        msg = ImageMessage(data=img, encoding="rgb8")

        data = msg.to_bytes()
        restored, _ = ImageMessage.from_bytes(data)

        np.testing.assert_array_equal(img, restored.data)
        assert restored.encoding == "rgb8"
        assert restored.width == 640
        assert restored.height == 480

    def test_point_cloud_message(self):
        """PointCloudMessage should work."""
        points = np.random.randn(1000, 3).astype(np.float32)
        colors = np.random.randint(0, 256, (1000, 3), dtype=np.uint8)

        msg = PointCloudMessage(points=points, colors=colors, frame_id="lidar")

        data = msg.to_bytes()
        restored, _ = PointCloudMessage.from_bytes(data)

        np.testing.assert_array_almost_equal(points, restored.points)
        np.testing.assert_array_equal(colors, restored.colors)
        assert restored.frame_id == "lidar"


class TestMessagePerformance:
    """Performance tests for message serialization."""

    def test_large_array_serialization(self):
        """Large arrays should serialize efficiently."""
        import time

        # 1 megabyte array
        arr = np.random.randn(1024, 1024).astype(np.float32)
        msg = ArrayMessage(data=arr)

        # Serialize
        start = time.time()
        data = msg.to_bytes()
        serialize_time = time.time() - start

        # Deserialize
        start = time.time()
        restored, _ = ArrayMessage.from_bytes(data)
        deserialize_time = time.time() - start

        # Should be reasonably fast (< 100ms for 4MB)
        assert serialize_time < 0.1, f"Serialize too slow: {serialize_time:.3f}s"
        assert deserialize_time < 0.1, f"Deserialize too slow: {deserialize_time:.3f}s"

        np.testing.assert_array_almost_equal(arr, restored.data)
