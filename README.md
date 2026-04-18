# Cortex

Lightweight Python pub/sub over ZeroMQ, for robotics and beyond.

**[Documentation](https://sudoRicheek.github.io/cortex/)** · [Quickstart](https://sudoRicheek.github.io/cortex/getting-started/quickstart/) · [API Reference](https://sudoRicheek.github.io/cortex/reference/)

## Overview

Cortex is a pub/sub communication layer built on ZeroMQ IPC. Nodes publish typed messages on named topics; subscribers receive them via async callbacks. A small discovery daemon handles endpoint resolution so publishers and subscribers find each other automatically.

- **Typed messages** with 64-bit fingerprint verification — no silent type mismatches
- **Zero-copy frames** for NumPy arrays and PyTorch tensors over IPC
- **uvloop-backed async** for low tail latency on Linux/macOS
- **Simple API**: `Node`, `Publisher`, `Subscriber`, rate-based `Executor`

```
┌──────────────────────────────────────┐
│           Discovery Daemon           │
│      ipc:///tmp/cortex_discovery     │
└──────┬───────────────────────┬───────┘
       │ REQ/REP (register)    │ REQ/REP (lookup)
┌──────┴──────┐         ┌──────┴──────┐
│  Publisher  │─PUB/SUB─│  Subscriber │
└─────────────┘   IPC   └─────────────┘
```

## Installation

```bash
git clone https://github.com/sudoRicheek/cortex.git
cd cortex
pip install -e "."          # core
pip install -e ".[torch]"   # + PyTorch
```

## Quick Start

```bash
cortex-discovery   # terminal 1: start the discovery daemon
```

```python
# publisher.py
import numpy as np
from cortex import Node, ArrayMessage

node = Node("sensor")
pub  = node.create_publisher("/sensor/data", ArrayMessage)
pub.publish(ArrayMessage(data=np.random.randn(640, 480, 3).astype("f4"), name="frame"))
```

```python
# subscriber.py
from cortex import Node, ArrayMessage

def on_msg(msg: ArrayMessage, header):
    print(f"got {msg.name}: {msg.data.shape}")

node = Node("proc")
node.create_subscriber("/sensor/data", ArrayMessage, callback=on_msg)
node.spin()
```

Custom message types, rate-based executors, multi-node systems — see the **[docs](https://sudoRicheek.github.io/cortex/)**.

## Messages

Define messages as plain dataclasses — registration, fingerprinting, and serialization are automatic:

```python
from dataclasses import dataclass
import numpy as np
from cortex.messages.base import Message

@dataclass
class RobotState(Message):
    position: np.ndarray    # zero-copy over IPC
    joint_angles: np.ndarray
    is_moving: bool
```

Built-ins cover the common cases: `StringMessage`, `ArrayMessage`, `ImageMessage`, `PointCloudMessage`, `PoseMessage`, `TensorMessage`, and more. See the [Messages reference](https://sudoRicheek.github.io/cortex/components/messages/).

## Examples

See the `examples/` directory for complete examples. One example:

```bash
python -m cortex.discovery.daemon   # Terminal 1
python examples/publisher_numpy.py  # Terminal 2
python examples/subscriber_numpy.py # Terminal 3
```

Full walkthroughs in the [Tutorials](https://sudoRicheek.github.io/cortex/tutorials/custom-messages/).

## Testing

```bash
pytest
```

## License

Apache 2.0 — see [LICENSE](LICENSE).
