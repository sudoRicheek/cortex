# Cortex

A lightweight framework for inter-process communication using ZeroMQ.

## Overview

Cortex provides a simple yet powerful way to build distributed systems in Python. It features:

- **Publisher/Subscriber pattern** for decoupled communication
- **Discovery service** for automatic topic resolution
- **IPC transport** using ZeroMQ for low-latency local communication
- **64-bit fingerprint hashing** for fast message type identification
- **Native support** for NumPy arrays, PyTorch tensors, and Python dictionaries
- **Simple API** with Node, Publisher, Subscriber, and Executor abstractions

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        Discovery Daemon                         │
│                   ipc:///tmp/cortex_discovery                   │
│                                                                 │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │                    Topic Registry                        │   │
│  │  /camera/image  -> ipc:///tmp/cortex/topics/camera_...   │   │
│  │  /robot/state   -> ipc:///tmp/cortex/topics/robot_...    │   │
│  └──────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
         ▲                                        ▲
         │ REQ/REP                                │ REQ/REP
         │ (register, lookup)                     │ (lookup)
         │                                        │
┌────────┴────────┐                     ┌─────────┴────────┐
│  Publisher Node │                     │  Subscriber Node │
│                 │                     │                  │
│  ┌───────────┐  │    PUB/SUB (IPC)    │  ┌───────────┐   │
│  │ Publisher ├──┼─────────────────────┼──► Subscriber│   │
│  └───────────┘  │                     │  └───────────┘   │
└─────────────────┘                     └──────────────────┘
```

## Installation

```bash
# Clone the repository
git clone https://github.com/sudoRicheek/cortex.git
cd cortex

# Install in development mode
pip install -e ".[dev]"

# With PyTorch support
pip install -e ".[all]"
```

## Quick Start

### 1. Start the Discovery Daemon

The discovery daemon must be running for publishers and subscribers to find each other:

```bash
# Start the discovery daemon
python -m cortex.discovery.daemon

# Or use the installed command
cortex-discovery
```

### 2. Create a Publisher

```python
import numpy as np
from cortex import Node, ArrayMessage

# Create a node
node = Node(name="sensor_node")

# Create a publisher
pub = node.create_publisher(
    topic_name="/sensor/data",
    message_type=ArrayMessage,
)

# Publish messages
data = np.random.randn(100, 100).astype(np.float32)
pub.publish(ArrayMessage(data=data, name="frame_0"))
```

### 3. Create a Subscriber

```python
from cortex import Node, ArrayMessage
from cortex.messages.base import MessageHeader

def on_data_received(msg: ArrayMessage, header: MessageHeader):
    print(f"Received: {msg.name}, shape={msg.data.shape}")

# Create a node
node = Node(name="processor_node")

# Create a subscriber
sub = node.create_subscriber(
    topic_name="/sensor/data",
    message_type=ArrayMessage,
    callback=on_data_received,
)

# Spin to process callbacks
node.spin()
```

## Message Types

### Built-in Messages

Cortex provides several built-in message types:

```python
from cortex.messages.standard import (
    StringMessage,    # Simple strings
    IntMessage,       # Integers
    FloatMessage,     # Floating point numbers
    DictMessage,      # Nested dictionaries
    ArrayMessage,     # NumPy arrays
    TensorMessage,    # PyTorch tensors
    ImageMessage,     # Image data
    PointCloudMessage,# 3D point clouds
    PoseMessage,      # 6DOF poses
)
```

### Custom Messages

Define your own messages using Python dataclasses:

```python
from dataclasses import dataclass
import numpy as np
from cortex.messages.base import Message

@dataclass
class RobotState(Message):
    timestamp: float
    position: np.ndarray  # [x, y, z]
    velocity: np.ndarray  # [vx, vy, vz]
    joint_angles: np.ndarray
    is_moving: bool
```

Custom messages are automatically:
- Registered with the type system
- Assigned a 64-bit fingerprint for fast identification
- Serialized/deserialized efficiently

## Working with Different Data Types

### NumPy Arrays

```python
from cortex import ArrayMessage
import numpy as np

# Create array message
arr = np.random.randn(480, 640, 3).astype(np.float32)
msg = ArrayMessage(data=arr, name="rgb_image", frame_id="camera")

# Publish
pub.publish(msg)
```

### PyTorch Tensors

```python
from cortex import TensorMessage
import torch

# Create tensor message
tensor = torch.randn(4, 256, 7, 7)
msg = TensorMessage(data=tensor, name="features")

# Tensors are automatically moved to CPU for serialization
# Device information is preserved
pub.publish(msg)
```

### Dictionaries (Nested Structures)

```python
from cortex import DictMessage
import numpy as np

# Complex nested data
state = {
    "timestamp": 1234567890.0,
    "pose": {
        "position": {"x": 1.0, "y": 2.0, "z": 0.0},
        "orientation": {"x": 0, "y": 0, "z": 0, "w": 1},
    },
    "joint_positions": np.array([0.1, 0.2, 0.3]),
    "status": {"is_moving": True, "battery": 85.5},
}

msg = DictMessage(data=state)
pub.publish(msg)
```

## Node API

### Creating Publishers and Subscribers

```python
from cortex import Node

node = Node(name="my_node")

# Publisher
pub = node.create_publisher("/topic", MessageType, queue_size=10)

# Subscriber with callback
sub = node.create_subscriber(
    "/topic",
    MessageType,
    callback=my_callback,
    wait_for_topic=True,  # Wait for publisher to appear
    topic_timeout=30.0,   # Timeout for waiting
)
```

### Using Timers

```python
def periodic_task():
    print("Timer fired!")

# Create a timer that fires every 100ms
node.create_timer(0.1, periodic_task)

# Spin to process timers and callbacks
node.spin()
```

### Using the Executor

For multi-node systems:

```python
from cortex.core.executor import SingleThreadedExecutor, MultiThreadedExecutor

# Single-threaded execution
executor = SingleThreadedExecutor()
executor.add_node(node1)
executor.add_node(node2)
executor.spin()

# Multi-threaded (each node in its own thread)
executor = MultiThreadedExecutor(num_threads=4)
executor.add_node(node1)
executor.add_node(node2)
executor.spin()
```

## Discovery Service

### Using the Discovery Client Directly

```python
from cortex.discovery import DiscoveryClient, TopicInfo

client = DiscoveryClient()

# List all topics
topics = client.list_topics()
for topic in topics:
    print(f"{topic.name} -> {topic.address}")

# Wait for a specific topic
topic_info = client.wait_for_topic("/camera/image", timeout=30.0)

# Manual topic registration
info = TopicInfo(
    name="/my/topic",
    address="ipc:///tmp/my_socket",
    message_type="MyMessage",
    fingerprint=12345,
    publisher_node="my_node"
)
client.register_topic(info)
```

## Message Fingerprinting

Each message type has a unique 64-bit fingerprint computed from:
- The fully qualified class name
- Field names and types

This allows fast message identification without parsing:

```python
from cortex.messages.base import Message
from cortex.utils.hashing import compute_fingerprint

@dataclass
class MyMessage(Message):
    value: int

# Get the fingerprint
fp = MyMessage.fingerprint()  # e.g., 0x1234567890ABCDEF

# Fingerprint is sent with every message for type verification
```

## Examples

See the `examples/` directory for complete examples:

- `publisher_numpy.py` / `subscriber_numpy.py` - NumPy array transfer
- `publisher_dict.py` / `subscriber_dict.py` - Dictionary messages
- `publisher_tensor.py` / `subscriber_tensor.py` - PyTorch tensor transfer
- `multi_node_system.py` - Complete multi-node system example

Run examples:

```bash
# Terminal 1
python -m cortex.discovery.daemon

# Terminal 2
python examples/publisher_numpy.py

# Terminal 3
python examples/subscriber_numpy.py
```

## Testing

```bash
# Run all tests
pytest

# Run with coverage
pytest --cov=cortex

# Run specific test file
pytest tests/test_messages.py -v
```

## Project Structure

```
cortex/
├── src/cortex/
│   ├── __init__.py           # Package exports
│   ├── core/
│   │   ├── node.py           # Node abstraction
│   │   ├── publisher.py      # Publisher implementation
│   │   ├── subscriber.py     # Subscriber implementation
│   │   └── executor.py       # Executor for multi-node
│   ├── discovery/
│   │   ├── daemon.py         # Discovery daemon
│   │   ├── client.py         # Discovery client
│   │   └── protocol.py       # Protocol definitions
│   ├── messages/
│   │   ├── base.py           # Base message class
│   │   └── standard.py       # Standard message types
│   └── utils/
│       ├── hashing.py        # Fingerprint computation
│       └── serialization.py  # Data serialization
├── tests/                    # Unit tests
├── examples/                 # Example applications
└── pyproject.toml           # Package configuration
```

## Performance

Cortex is designed for lightweight, high-frequency communication:

- **IPC transport**: ZeroMQ IPC sockets for minimal latency
- **Zero-copy where possible**: NumPy arrays use efficient byte views
- **Fingerprint-based dispatch**: O(1) message type lookup
- **Minimal overhead**: Simple binary protocol without XML/JSON parsing

Typical latencies on a modern system:
- Small messages (< 1KB): < 100 µs
- Large arrays (1 MB): < 5 ms

## License

Apache License 2.0 - see the [LICENSE](LICENSE) file for details.
