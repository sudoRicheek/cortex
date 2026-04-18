# NumPy arrays & images

Cortex treats NumPy arrays as first-class payloads. Array bytes travel as
separate ZMQ frames and are reconstructed with `np.frombuffer` on the
receiver — no intermediate `bytes` object, no extra copy.

## Pattern: publisher that emits synthetic frames

```python title="camera.py"
import numpy as np
import cortex
from cortex import Node, ArrayMessage


class Camera(Node):
    def __init__(self):
        super().__init__("camera")
        self.pub = self.create_publisher("/cam/frame", ArrayMessage)
        self.create_timer(1 / 30, self.tick)  # 30 fps
        self._i = 0

    async def tick(self):
        # Synthetic 640x480 RGB frame
        frame = (np.random.rand(480, 640, 3) * 255).astype("uint8")
        self.pub.publish(ArrayMessage(data=frame, name=f"f{self._i}", frame_id="camera"))
        self._i += 1


cortex.run(Camera().run())
```

## Pattern: subscriber that processes frames

```python title="viewer.py"
import numpy as np
import cortex
from cortex import Node, ArrayMessage
from cortex.messages.base import MessageHeader


async def on_frame(msg: ArrayMessage, header: MessageHeader):
    # msg.data aliases the ZMQ frame buffer — copy before mutating
    frame = msg.data.copy()
    frame[..., 0] = 0   # zero out red channel
    print(f"[{header.sequence}] {msg.name} mean={frame.mean():.1f}")


class Viewer(Node):
    def __init__(self):
        super().__init__("viewer")
        self.create_subscriber("/cam/frame", ArrayMessage, callback=on_frame)


cortex.run(Viewer().run())
```

## Aliasing rule of thumb

```mermaid
flowchart LR
    A[recv multipart<br/>copy=False] --> B[np.frombuffer view]
    B --> C{Do you...}
    C -->|only read inside callback| OK[Use as-is: fastest]
    C -->|mutate| CP[arr = arr.copy]
    C -->|keep past callback| CP
    C -->|pass to another thread| CP
    CP --> Safe[safe, owned copy]
```

## `ImageMessage` specifics

[`ImageMessage`][cortex.messages.standard.ImageMessage] carries an `encoding`
string plus optional `width` / `height` (auto-filled from the array shape):

```python
from cortex.messages.standard import ImageMessage

msg = ImageMessage(data=frame, encoding="rgb8")  # width/height filled on __post_init__
pub.publish(msg)
```

Encodings are free-form strings — Cortex does no validation or conversion.
Downstream code decides what `rgb8` / `bgr8` / `mono8` mean.

## Zero-copy footprint

A 1080p RGB frame is ~6 MB. On the benchmark suite:

- Allocation on encode: **zero** (array is passed by view).
- Allocation on decode: **zero** (array is a view into the ZMQ frame).
- Throughput: ~1400 fps on a modern workstation.

## See also

- [Concepts → Message wire format](../concepts/message-wire-format.md)
- [Components → Serialization](../components/serialization.md)
- [Guides → Performance tuning](../guides/performance-tuning.md)
