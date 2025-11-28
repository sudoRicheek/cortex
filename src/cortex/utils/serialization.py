"""
Serialization utilities for Cortex messages.

Supports efficient serialization of:
- NumPy arrays
- PyTorch tensors (optional)
- Python dictionaries
- Primitive types
"""

import struct
from enum import IntEnum
from typing import Any

import msgpack
import numpy as np

# Optional torch support
try:
    import torch

    TORCH_AVAILABLE = True
except ImportError:
    torch = None
    TORCH_AVAILABLE = False


class DataType(IntEnum):
    """Type identifiers for serialized data."""

    NONE = 0
    PRIMITIVE = 1  # int, float, str, bool, None
    NUMPY = 2
    TORCH = 3
    DICT = 4
    LIST = 5
    BYTES = 6


def serialize_numpy(arr: np.ndarray) -> bytes:
    """
    Serialize a NumPy array to bytes.

    Format:
    - 1 byte: number of dimensions
    - 4 bytes per dim: shape
    - variable: dtype string length (2 bytes) + dtype string
    - remaining: raw array data
    """
    ndim = arr.ndim
    shape = arr.shape
    dtype_str = str(arr.dtype).encode("utf-8")

    # Pack header: ndim (1 byte) + shape (4 bytes each) + dtype
    header = struct.pack(f">B{ndim}I", ndim, *shape)
    header += struct.pack(">H", len(dtype_str)) + dtype_str

    # Get contiguous array data
    data = np.ascontiguousarray(arr).tobytes()

    return header + data


def deserialize_numpy(data: bytes) -> tuple[np.ndarray, int]:
    """
    Deserialize bytes to a NumPy array.

    Returns:
        Tuple of (array, bytes_consumed)
    """
    offset = 0

    # Read ndim
    ndim = struct.unpack(">B", data[offset : offset + 1])[0]
    offset += 1

    # Read shape
    shape = struct.unpack(f">{ndim}I", data[offset : offset + 4 * ndim])
    offset += 4 * ndim

    # Read dtype
    dtype_len = struct.unpack(">H", data[offset : offset + 2])[0]
    offset += 2
    dtype_str = data[offset : offset + dtype_len].decode("utf-8")
    offset += dtype_len

    # Calculate data size and read
    dtype = np.dtype(dtype_str)
    size = int(np.prod(shape)) * dtype.itemsize
    arr_data = data[offset : offset + size]
    offset += size

    arr = np.frombuffer(arr_data, dtype=dtype).reshape(shape)
    return arr.copy(), offset


def serialize_torch(tensor: "torch.Tensor") -> bytes:
    """
    Serialize a PyTorch tensor to bytes.

    Converts to NumPy for serialization, preserving device and requires_grad info.
    """
    if not TORCH_AVAILABLE:
        raise RuntimeError("PyTorch is not available")

    # Store metadata
    device_str = str(tensor.device).encode("utf-8")
    requires_grad = tensor.requires_grad

    # Convert to numpy (move to CPU if needed)
    arr = tensor.detach().cpu().numpy()

    # Pack metadata
    meta = struct.pack(">?H", requires_grad, len(device_str)) + device_str

    # Serialize the numpy array
    arr_bytes = serialize_numpy(arr)

    return meta + arr_bytes


def deserialize_torch(data: bytes) -> tuple["torch.Tensor", int]:
    """
    Deserialize bytes to a PyTorch tensor.

    Returns:
        Tuple of (tensor, bytes_consumed)
    """
    if not TORCH_AVAILABLE:
        raise RuntimeError("PyTorch is not available")

    offset = 0

    # Read metadata
    requires_grad = struct.unpack(">?", data[offset : offset + 1])[0]
    offset += 1
    device_len = struct.unpack(">H", data[offset : offset + 2])[0]
    offset += 2
    device_str = data[offset : offset + device_len].decode("utf-8")
    offset += device_len

    # Deserialize numpy array
    arr, arr_bytes = deserialize_numpy(data[offset:])
    offset += arr_bytes

    # Convert to tensor
    tensor = torch.from_numpy(arr)

    # Restore device (only if available)
    if device_str.startswith("cuda") and torch.cuda.is_available():
        tensor = tensor.to(device_str)

    if requires_grad:
        tensor.requires_grad_(True)

    return tensor, offset


def serialize(value: Any) -> bytes:
    """
    Serialize any supported value to bytes.

    Supported types:
    - None, int, float, str, bool
    - bytes
    - list, dict
    - numpy.ndarray
    - torch.Tensor
    """
    if value is None:
        return struct.pack(">B", DataType.NONE)

    if isinstance(value, np.ndarray):
        type_byte = struct.pack(">B", DataType.NUMPY)
        return type_byte + serialize_numpy(value)

    if TORCH_AVAILABLE and isinstance(value, torch.Tensor):
        type_byte = struct.pack(">B", DataType.TORCH)
        return type_byte + serialize_torch(value)

    if isinstance(value, bytes):
        type_byte = struct.pack(">B", DataType.BYTES)
        length = struct.pack(">I", len(value))
        return type_byte + length + value

    if isinstance(value, dict):
        type_byte = struct.pack(">B", DataType.DICT)
        # Recursively serialize dict values
        serialized_dict = {}
        for k, v in value.items():
            serialized_dict[k] = serialize(v)
        packed = msgpack.packb(serialized_dict, use_bin_type=True)
        return type_byte + struct.pack(">I", len(packed)) + packed

    if isinstance(value, (list, tuple)):
        type_byte = struct.pack(">B", DataType.LIST)
        # Recursively serialize list items
        serialized_list = [serialize(item) for item in value]
        packed = msgpack.packb(serialized_list, use_bin_type=True)
        return type_byte + struct.pack(">I", len(packed)) + packed

    # Primitive types: use msgpack
    type_byte = struct.pack(">B", DataType.PRIMITIVE)
    packed = msgpack.packb(value, use_bin_type=True)
    return type_byte + struct.pack(">I", len(packed)) + packed


def deserialize(data: bytes) -> tuple[Any, int]:
    """
    Deserialize bytes to a value.

    Returns:
        Tuple of (value, bytes_consumed)
    """
    offset = 0
    data_type = DataType(struct.unpack(">B", data[offset : offset + 1])[0])
    offset += 1

    if data_type == DataType.NONE:
        return None, offset

    if data_type == DataType.NUMPY:
        arr, arr_bytes = deserialize_numpy(data[offset:])
        return arr, offset + arr_bytes

    if data_type == DataType.TORCH:
        tensor, tensor_bytes = deserialize_torch(data[offset:])
        return tensor, offset + tensor_bytes

    if data_type == DataType.BYTES:
        length = struct.unpack(">I", data[offset : offset + 4])[0]
        offset += 4
        return data[offset : offset + length], offset + length

    if data_type == DataType.DICT:
        length = struct.unpack(">I", data[offset : offset + 4])[0]
        offset += 4
        packed = data[offset : offset + length]
        offset += length

        serialized_dict = msgpack.unpackb(packed, raw=False)
        result = {}
        for k, v in serialized_dict.items():
            result[k], _ = deserialize(v)
        return result, offset

    if data_type == DataType.LIST:
        length = struct.unpack(">I", data[offset : offset + 4])[0]
        offset += 4
        packed = data[offset : offset + length]
        offset += length

        serialized_list = msgpack.unpackb(packed, raw=False)
        result = []
        for item in serialized_list:
            val, _ = deserialize(item)
            result.append(val)
        return result, offset

    if data_type == DataType.PRIMITIVE:
        length = struct.unpack(">I", data[offset : offset + 4])[0]
        offset += 4
        packed = data[offset : offset + length]
        offset += length
        return msgpack.unpackb(packed, raw=False), offset

    raise ValueError(f"Unknown data type: {data_type}")


def serialize_message_data(fields: dict[str, Any]) -> bytes:
    """
    Serialize message fields to bytes.

    Format:
    - 2 bytes: number of fields
    - For each field:
        - 2 bytes: key length
        - key bytes
        - 4 bytes: value length
        - value bytes
    """
    parts = [struct.pack(">H", len(fields))]

    for key, value in fields.items():
        key_bytes = key.encode("utf-8")
        value_bytes = serialize(value)

        parts.append(struct.pack(">H", len(key_bytes)))
        parts.append(key_bytes)
        parts.append(struct.pack(">I", len(value_bytes)))
        parts.append(value_bytes)

    return b"".join(parts)


def deserialize_message_data(data: bytes) -> dict[str, Any]:
    """Deserialize bytes to message fields."""
    offset = 0
    num_fields = struct.unpack(">H", data[offset : offset + 2])[0]
    offset += 2

    fields = {}
    for _ in range(num_fields):
        key_len = struct.unpack(">H", data[offset : offset + 2])[0]
        offset += 2
        key = data[offset : offset + key_len].decode("utf-8")
        offset += key_len

        value_len = struct.unpack(">I", data[offset : offset + 4])[0]
        offset += 4
        value_bytes = data[offset : offset + value_len]
        offset += value_len

        value, _ = deserialize(value_bytes)
        fields[key] = value

    return fields
