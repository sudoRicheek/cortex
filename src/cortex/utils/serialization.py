"""Serialization utilities for Cortex messages."""

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


_NUMPY_EXT_CODE = 1
_TORCH_EXT_CODE = 2
_OOB_MARKER = "__cortex_oob__"


def _as_buffer_view(data: Any) -> memoryview:
    """Return a memoryview for bytes-like objects and ZMQ frames."""
    if hasattr(data, "buffer"):
        return memoryview(data.buffer)
    return memoryview(data)


def _encode_numpy_payload(arr: np.ndarray) -> bytes:
    """Encode a NumPy array payload for msgpack ext transport."""
    contiguous = np.ascontiguousarray(arr)
    payload = (
        contiguous.dtype.str,
        contiguous.shape,
        contiguous.tobytes(order="C"),
    )
    return msgpack.packb(payload, use_bin_type=True)


def _decode_numpy_payload(payload: bytes) -> np.ndarray:
    """Decode a NumPy array payload from msgpack ext transport."""
    dtype_str, shape, raw = msgpack.unpackb(payload, raw=False)
    array = np.frombuffer(raw, dtype=np.dtype(dtype_str)).reshape(tuple(shape))
    return array


def _msgpack_default(value: Any) -> msgpack.ExtType:
    """Msgpack default hook with array/tensor support."""
    if isinstance(value, np.ndarray):
        return msgpack.ExtType(_NUMPY_EXT_CODE, _encode_numpy_payload(value))

    if TORCH_AVAILABLE and isinstance(value, torch.Tensor):
        device_str = str(value.device)
        requires_grad = value.requires_grad
        contiguous = np.ascontiguousarray(value.detach().cpu().numpy())
        payload = msgpack.packb(
            (
                device_str,
                requires_grad,
                contiguous.dtype.str,
                contiguous.shape,
                contiguous.tobytes(order="C"),
            ),
            use_bin_type=True,
        )
        return msgpack.ExtType(_TORCH_EXT_CODE, payload)

    raise TypeError(f"Unsupported type for serialization: {type(value)!r}")


def _msgpack_ext_hook(code: int, payload: bytes) -> Any:
    """Msgpack ext hook with array/tensor support."""
    if code == _NUMPY_EXT_CODE:
        return _decode_numpy_payload(payload)

    if code == _TORCH_EXT_CODE:
        if not TORCH_AVAILABLE:
            raise RuntimeError("PyTorch is not available")

        device_str, requires_grad, dtype_str, shape, raw = msgpack.unpackb(
            payload, raw=False
        )
        array = np.frombuffer(raw, dtype=np.dtype(dtype_str)).reshape(tuple(shape))
        tensor = torch.from_numpy(array.copy())
        if device_str.startswith("cuda") and torch.cuda.is_available():
            tensor = tensor.to(device_str)
        if requires_grad:
            tensor.requires_grad_(True)
        return tensor

    return msgpack.ExtType(code, payload)


def _encode_transport_value(value: Any, buffers: list[Any]) -> Any:
    """Replace large buffers with out-of-band descriptors for transport."""
    if isinstance(value, np.ndarray):
        contiguous = np.ascontiguousarray(value)
        buffer_index = len(buffers)
        buffers.append(contiguous)
        return {
            _OOB_MARKER: "numpy",
            "buffer": buffer_index,
            "dtype": contiguous.dtype.str,
            "shape": list(contiguous.shape),
        }

    if TORCH_AVAILABLE and isinstance(value, torch.Tensor):
        contiguous = np.ascontiguousarray(value.detach().cpu().numpy())
        buffer_index = len(buffers)
        buffers.append(contiguous)
        return {
            _OOB_MARKER: "torch",
            "buffer": buffer_index,
            "dtype": contiguous.dtype.str,
            "shape": list(contiguous.shape),
            "device": str(value.device),
            "requires_grad": value.requires_grad,
        }

    if isinstance(value, dict):
        return {key: _encode_transport_value(item, buffers) for key, item in value.items()}

    if isinstance(value, (list, tuple)):
        return [_encode_transport_value(item, buffers) for item in value]

    return value


def _decode_transport_value(value: Any, buffers: list[Any]) -> Any:
    """Restore out-of-band transport descriptors back into Python values."""
    if isinstance(value, dict) and _OOB_MARKER in value:
        buffer_index = value["buffer"]
        buffer_view = _as_buffer_view(buffers[buffer_index])
        shape = tuple(value["shape"])
        array = np.frombuffer(buffer_view, dtype=np.dtype(value["dtype"])).reshape(shape)

        if value[_OOB_MARKER] == "numpy":
            return array

        if value[_OOB_MARKER] == "torch":
            if not TORCH_AVAILABLE:
                raise RuntimeError("PyTorch is not available")
            tensor = torch.from_numpy(array.copy())
            device_str = value["device"]
            if device_str.startswith("cuda") and torch.cuda.is_available():
                tensor = tensor.to(device_str)
            if value["requires_grad"]:
                tensor.requires_grad_(True)
            return tensor

    if isinstance(value, dict):
        return {key: _decode_transport_value(item, buffers) for key, item in value.items()}

    if isinstance(value, list):
        return [_decode_transport_value(item, buffers) for item in value]

    return value


def serialize_numpy(arr: np.ndarray) -> bytes:
    """
    Serialize a NumPy array to bytes.

    Format:
    - 1 byte: number of dimensions
    - 4 bytes per dim: shape
    - variable: dtype string length (2 bytes) + dtype string
    - remaining: raw array data
    """
    contiguous = np.ascontiguousarray(arr)
    ndim = contiguous.ndim
    dtype_str = contiguous.dtype.str.encode("utf-8")

    header_size = 1 + (4 * ndim) + 2 + len(dtype_str)
    header = bytearray(header_size)
    struct.pack_into(f">B{ndim}I", header, 0, ndim, *contiguous.shape)
    offset = 1 + (4 * ndim)
    struct.pack_into(">H", header, offset, len(dtype_str))
    offset += 2
    header[offset:] = dtype_str

    return bytes(header) + contiguous.tobytes(order="C")


def deserialize_numpy(data: bytes | memoryview, *, copy: bool = False) -> tuple[np.ndarray, int]:
    """
    Deserialize bytes to a NumPy array.

    Returns:
        Tuple of (array, bytes_consumed)
    """
    offset = 0
    view = _as_buffer_view(data)

    # Read ndim
    ndim = struct.unpack(">B", view[offset : offset + 1])[0]
    offset += 1

    # Read shape
    shape = struct.unpack(f">{ndim}I", view[offset : offset + 4 * ndim])
    offset += 4 * ndim

    # Read dtype
    dtype_len = struct.unpack(">H", view[offset : offset + 2])[0]
    offset += 2
    dtype_str = bytes(view[offset : offset + dtype_len]).decode("utf-8")
    offset += dtype_len

    # Calculate data size and read
    dtype = np.dtype(dtype_str)
    size = int(np.prod(shape)) * dtype.itemsize
    arr_data = view[offset : offset + size]
    offset += size

    arr = np.frombuffer(arr_data, dtype=dtype).reshape(shape)
    if copy:
        arr = arr.copy()
    return arr, offset


def serialize_torch(tensor: Any) -> bytes:
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


def deserialize_torch(data: bytes) -> tuple[Any, int]:
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
    arr, arr_bytes = deserialize_numpy(data[offset:], copy=True)
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
        return struct.pack(">B", DataType.NUMPY) + serialize_numpy(value)

    if TORCH_AVAILABLE and isinstance(value, torch.Tensor):
        return struct.pack(">B", DataType.TORCH) + serialize_torch(value)

    if isinstance(value, bytes):
        return struct.pack(">BI", DataType.BYTES, len(value)) + value

    if isinstance(value, dict):
        packed = msgpack.packb(value, default=_msgpack_default, use_bin_type=True)
        return struct.pack(">BI", DataType.DICT, len(packed)) + packed

    if isinstance(value, (list, tuple)):
        packed = msgpack.packb(value, default=_msgpack_default, use_bin_type=True)
        return struct.pack(">BI", DataType.LIST, len(packed)) + packed

    packed = msgpack.packb(value, use_bin_type=True)
    return struct.pack(">BI", DataType.PRIMITIVE, len(packed)) + packed


def deserialize(data: bytes) -> tuple[Any, int]:
    """
    Deserialize bytes to a value.

    Returns:
        Tuple of (value, bytes_consumed)
    """
    offset = 0
    view = _as_buffer_view(data)
    data_type = DataType(struct.unpack(">B", view[offset : offset + 1])[0])
    offset += 1

    if data_type == DataType.NONE:
        return None, offset

    if data_type == DataType.NUMPY:
        arr, arr_bytes = deserialize_numpy(view[offset:])
        return arr, offset + arr_bytes

    if data_type == DataType.TORCH:
        tensor, tensor_bytes = deserialize_torch(bytes(view[offset:]))
        return tensor, offset + tensor_bytes

    if data_type == DataType.BYTES:
        length = struct.unpack(">I", view[offset : offset + 4])[0]
        offset += 4
        return bytes(view[offset : offset + length]), offset + length

    if data_type in (DataType.DICT, DataType.LIST, DataType.PRIMITIVE):
        length = struct.unpack(">I", view[offset : offset + 4])[0]
        offset += 4
        payload = view[offset : offset + length]
        value = msgpack.unpackb(payload, raw=False, ext_hook=_msgpack_ext_hook)
        return value, offset + length

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
    return serialize(fields)


def deserialize_message_data(data: bytes) -> dict[str, Any]:
    """Deserialize bytes to message fields."""
    fields, _ = deserialize(data)
    return fields


def serialize_message_values(values: list[Any] | tuple[Any, ...]) -> bytes:
    """Serialize ordered message field values."""
    return serialize(list(values))


def deserialize_message_values(data: bytes | memoryview) -> list[Any]:
    """Deserialize ordered message field values."""
    values, _ = deserialize(_as_buffer_view(data))
    return values


def serialize_message_frames(values: list[Any] | tuple[Any, ...]) -> list[Any]:
    """Serialize message values into metadata plus out-of-band buffer frames."""
    buffers: list[Any] = []
    encoded_values = [_encode_transport_value(value, buffers) for value in values]
    metadata = msgpack.packb(encoded_values, use_bin_type=True)
    return [metadata, *buffers]


def deserialize_message_frames(frames: list[Any]) -> list[Any]:
    """Deserialize metadata plus out-of-band buffer frames into message values."""
    if not frames:
        return []

    encoded_values = msgpack.unpackb(_as_buffer_view(frames[0]), raw=False)
    return [_decode_transport_value(value, frames[1:]) for value in encoded_values]
