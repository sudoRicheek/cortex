"""
Standard message types for Cortex.

These are commonly used message types that support numpy arrays,
torch tensors, and Python dictionaries.
"""

from dataclasses import dataclass
from typing import Any

import numpy as np

from cortex.messages.base import Message

# Optional torch support
try:
    import torch

    TORCH_AVAILABLE = True
except ImportError:
    torch = None
    TORCH_AVAILABLE = False


@dataclass
class StringMessage(Message):
    """Simple string message."""

    data: str


@dataclass
class IntMessage(Message):
    """Simple integer message."""

    data: int


@dataclass
class FloatMessage(Message):
    """Simple float message."""

    data: float


@dataclass
class BytesMessage(Message):
    """Raw bytes message."""

    data: bytes


@dataclass
class DictMessage(Message):
    """
    Dictionary message supporting nested structures.

    Values can be primitives, numpy arrays, torch tensors, or nested dicts/lists.
    """

    data: dict[str, Any]


@dataclass
class ListMessage(Message):
    """List message supporting mixed types."""

    data: list[Any]


@dataclass
class ArrayMessage(Message):
    """
    NumPy array message.

    Efficiently serializes numpy arrays of any dtype and shape.
    """

    data: np.ndarray

    # Optional metadata
    name: str = ""
    frame_id: str = ""


@dataclass
class MultiArrayMessage(Message):
    """
    Multiple NumPy arrays message.

    Useful for sending related arrays together (e.g., points + colors).
    """

    arrays: dict[str, np.ndarray]

    # Optional metadata
    frame_id: str = ""


@dataclass
class TensorMessage(Message):
    """
    PyTorch tensor message.

    Preserves tensor device and requires_grad attributes.
    Note: Tensors are moved to CPU for serialization.
    """

    data: Any  # torch.Tensor, but using Any to avoid import issues

    # Optional metadata
    name: str = ""

    def __post_init__(self):
        """Validate that data is a torch tensor if torch is available."""
        if TORCH_AVAILABLE and not isinstance(self.data, torch.Tensor):
            raise TypeError(f"Expected torch.Tensor, got {type(self.data)}")


@dataclass
class MultiTensorMessage(Message):
    """
    Multiple PyTorch tensors message.

    Useful for sending model inputs/outputs together.
    """

    tensors: dict[str, Any]  # Dict[str, torch.Tensor]


@dataclass
class ImageMessage(Message):
    """
    Image message using numpy array.

    Supports common image formats (HWC or CHW layout).
    """

    data: np.ndarray  # Image data as numpy array
    encoding: str = "rgb8"  # e.g., "rgb8", "bgr8", "mono8", "rgba8"
    width: int = 0
    height: int = 0

    def __post_init__(self):
        """Auto-fill width and height from array shape."""
        if self.width == 0 and self.data is not None:
            if self.data.ndim == 3:
                self.height, self.width = self.data.shape[:2]
            elif self.data.ndim == 2:
                self.height, self.width = self.data.shape


@dataclass
class PointCloudMessage(Message):
    """
    Point cloud message.

    Stores 3D points with optional attributes like colors and intensity.
    """

    points: np.ndarray  # Nx3 array of XYZ coordinates
    colors: np.ndarray | None = None  # Nx3 array of RGB colors (0-255)
    intensity: np.ndarray | None = None  # Nx1 array of intensity values
    normals: np.ndarray | None = None  # Nx3 array of normal vectors
    frame_id: str = ""


@dataclass
class PoseMessage(Message):
    """
    6DOF pose message.

    Represents position and orientation in 3D space.
    """

    position: np.ndarray  # [x, y, z]
    orientation: np.ndarray  # [qx, qy, qz, qw] quaternion
    frame_id: str = ""
    child_frame_id: str = ""


@dataclass
class TransformMessage(Message):
    """
    Transformation matrix message.

    4x4 homogeneous transformation matrix.
    """

    matrix: np.ndarray  # 4x4 transformation matrix
    frame_id: str = ""
    child_frame_id: str = ""


@dataclass
class TimestampMessage(Message):
    """
    Timestamp message for synchronization.
    """

    sec: int
    nanosec: int

    @classmethod
    def now(cls) -> "TimestampMessage":
        """Create a timestamp for the current time."""
        import time

        t = time.time_ns()
        return cls(sec=t // 1_000_000_000, nanosec=t % 1_000_000_000)


@dataclass
class HeaderMessage(Message):
    """
    Header message with timestamp and frame info.

    Similar to ROS std_msgs/Header.
    """

    stamp_sec: int
    stamp_nanosec: int
    frame_id: str = ""
    sequence: int = 0
