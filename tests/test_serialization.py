"""
Tests for serialization utilities.
"""

import numpy as np
import pytest

from cortex.utils.serialization import (
    TORCH_AVAILABLE,
    deserialize,
    deserialize_message_data,
    deserialize_numpy,
    serialize,
    serialize_message_data,
    serialize_numpy,
)

if TORCH_AVAILABLE:
    import torch


class TestNumpySerialization:
    """Tests for NumPy array serialization."""

    def test_serialize_1d_array(self):
        """1D arrays should serialize/deserialize correctly."""
        arr = np.array([1, 2, 3, 4, 5], dtype=np.int32)

        data = serialize_numpy(arr)
        restored, _ = deserialize_numpy(data)

        np.testing.assert_array_equal(arr, restored)
        assert arr.dtype == restored.dtype

    def test_serialize_2d_array(self):
        """2D arrays should serialize/deserialize correctly."""
        arr = np.random.randn(10, 20).astype(np.float32)

        data = serialize_numpy(arr)
        restored, _ = deserialize_numpy(data)

        np.testing.assert_array_almost_equal(arr, restored)
        assert arr.dtype == restored.dtype
        assert arr.shape == restored.shape

    def test_serialize_3d_array(self):
        """3D arrays (like images) should work."""
        arr = np.random.randint(0, 256, (480, 640, 3), dtype=np.uint8)

        data = serialize_numpy(arr)
        restored, _ = deserialize_numpy(data)

        np.testing.assert_array_equal(arr, restored)

    def test_various_dtypes(self):
        """Various numpy dtypes should work."""
        dtypes = [
            np.float32,
            np.float64,
            np.int8,
            np.int16,
            np.int32,
            np.int64,
            np.uint8,
            np.uint16,
            np.uint32,
            np.uint64,
            np.bool_,
        ]

        for dtype in dtypes:
            arr = np.array([1, 2, 3], dtype=dtype)
            data = serialize_numpy(arr)
            restored, _ = deserialize_numpy(data)

            np.testing.assert_array_equal(arr, restored)
            assert arr.dtype == restored.dtype, f"dtype mismatch for {dtype}"


class TestGenericSerialization:
    """Tests for generic serialize/deserialize functions."""

    def test_serialize_none(self):
        """None should serialize correctly."""
        data = serialize(None)
        restored, _ = deserialize(data)
        assert restored is None

    def test_serialize_int(self):
        """Integers should serialize correctly."""
        for val in [0, 1, -1, 2**31, -(2**31), 2**63 - 1]:
            data = serialize(val)
            restored, _ = deserialize(data)
            assert restored == val

    def test_serialize_float(self):
        """Floats should serialize correctly."""
        for val in [0.0, 1.5, -1.5, 3.14159, float("inf"), float("-inf")]:
            data = serialize(val)
            restored, _ = deserialize(data)
            assert restored == val or (np.isnan(val) and np.isnan(restored))

    def test_serialize_string(self):
        """Strings should serialize correctly."""
        for val in ["", "hello", "hello world", "🎉 unicode"]:
            data = serialize(val)
            restored, _ = deserialize(data)
            assert restored == val

    def test_serialize_bool(self):
        """Booleans should serialize correctly."""
        for val in [True, False]:
            data = serialize(val)
            restored, _ = deserialize(data)
            assert restored == val

    def test_serialize_bytes(self):
        """Bytes should serialize correctly."""
        val = b"\x00\x01\x02\xff"
        data = serialize(val)
        restored, _ = deserialize(data)
        assert restored == val

    def test_serialize_list(self):
        """Lists should serialize correctly."""
        val = [1, 2.0, "three", True, None]
        data = serialize(val)
        restored, _ = deserialize(data)
        assert restored == val

    def test_serialize_nested_list(self):
        """Nested lists should work."""
        val = [1, [2, [3, [4]]]]
        data = serialize(val)
        restored, _ = deserialize(data)
        assert restored == val

    def test_serialize_dict(self):
        """Dictionaries should serialize correctly."""
        val = {"a": 1, "b": 2.0, "c": "three"}
        data = serialize(val)
        restored, _ = deserialize(data)
        assert restored == val

    def test_serialize_nested_dict(self):
        """Nested dictionaries should work."""
        val = {"level1": {"level2": {"value": 42}}}
        data = serialize(val)
        restored, _ = deserialize(data)
        assert restored == val

    def test_serialize_numpy_in_dict(self):
        """NumPy arrays inside dicts should work."""
        arr = np.array([1, 2, 3], dtype=np.float32)
        val = {"array": arr, "name": "test"}

        data = serialize(val)
        restored, _ = deserialize(data)

        np.testing.assert_array_equal(arr, restored["array"])
        assert restored["name"] == "test"

    def test_serialize_numpy_via_generic(self):
        """NumPy arrays via generic serialize should work."""
        arr = np.random.randn(5, 5).astype(np.float64)

        data = serialize(arr)
        restored, _ = deserialize(data)

        np.testing.assert_array_almost_equal(arr, restored)


@pytest.mark.skipif(not TORCH_AVAILABLE, reason="PyTorch not available")
class TestTorchSerialization:
    """Tests for PyTorch tensor serialization."""

    def test_serialize_1d_tensor(self):
        """1D tensors should serialize correctly."""
        tensor = torch.tensor([1.0, 2.0, 3.0])

        data = serialize(tensor)
        restored, _ = deserialize(data)

        torch.testing.assert_close(tensor, restored)

    def test_serialize_2d_tensor(self):
        """2D tensors should serialize correctly."""
        tensor = torch.randn(10, 20)

        data = serialize(tensor)
        restored, _ = deserialize(data)

        torch.testing.assert_close(tensor, restored)

    def test_tensor_in_dict(self):
        """Tensors inside dicts should work."""
        tensor = torch.randn(5, 5)
        val = {"tensor": tensor, "name": "test"}

        data = serialize(val)
        restored, _ = deserialize(data)

        torch.testing.assert_close(tensor, restored["tensor"])
        assert restored["name"] == "test"


class TestMessageDataSerialization:
    """Tests for message data serialization."""

    def test_serialize_simple_fields(self):
        """Simple field types should work."""
        fields = {
            "int_field": 42,
            "float_field": 3.14,
            "str_field": "hello",
        }

        data = serialize_message_data(fields)
        restored = deserialize_message_data(data)

        assert restored == fields

    def test_serialize_array_fields(self):
        """NumPy array fields should work."""
        fields = {
            "data": np.random.randn(10, 10).astype(np.float32),
            "name": "test_array",
        }

        data = serialize_message_data(fields)
        restored = deserialize_message_data(data)

        np.testing.assert_array_almost_equal(fields["data"], restored["data"])
        assert fields["name"] == restored["name"]

    def test_serialize_mixed_fields(self):
        """Mixed field types should work."""
        fields = {
            "array": np.array([1, 2, 3]),
            "dict": {"nested": True},
            "list": [1, 2, 3],
            "string": "test",
        }

        data = serialize_message_data(fields)
        restored = deserialize_message_data(data)

        np.testing.assert_array_equal(fields["array"], restored["array"])
        assert fields["dict"] == restored["dict"]
        assert fields["list"] == restored["list"]
        assert fields["string"] == restored["string"]
