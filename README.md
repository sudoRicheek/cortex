# Cortex

Lightweight Python pub/sub over ZeroMQ, for robotics and beyond.

**[Documentation](https://sudoRicheek.github.io/cortex/)** · [Quickstart](https://sudoRicheek.github.io/cortex/getting-started/quickstart/) · [API Reference](https://sudoRicheek.github.io/cortex/reference/)

## Overview

Cortex is a pub/sub communication layer built on ZeroMQ IPC. Nodes publish typed messages on named topics; subscribers receive them via async callbacks. A small discovery daemon handles endpoint resolution so publishers and subscribers find each other automatically.

- **Typed messages** with 64-bit fingerprint verification — no silent type mismatches
- **Zero-copy frames** for NumPy arrays and PyTorch tensors over IPC
- **uvloop-backed async** for low tail latency on Linux/macOS
- **Sync mode** for fast pub-sub and free-threaded python314t
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

Run each block in its own terminal.

```bash
# terminal 1
cortex-discovery
```

```python
# terminal 2 — publisher.py
import numpy as np
import cortex
from cortex import Node
from cortex.messages.standard import ArrayMessage

async def main():
    async with Node("sensor") as node:
        pub = node.create_publisher("/sensor/data", ArrayMessage)
        i = 0

        async def tick():
            nonlocal i
            pub.publish(ArrayMessage(data=np.random.randn(64, 64).astype("f4"), name=f"frame_{i}"))
            i += 1

        node.create_timer(1 / 10, tick)
        await node.run()

cortex.run(main())
```

```python
# terminal 3 — subscriber.py
import cortex
from cortex import Node
from cortex.messages.standard import ArrayMessage

count = 0

async def on_msg(msg, header):
    global count
    count += 1
    print(f"[{count}] got {msg.name}: {msg.data.shape}")

async def main():
    async with Node("proc") as node:
        node.create_subscriber("/sensor/data", ArrayMessage, callback=on_msg)
        await node.run()

cortex.run(main())
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
