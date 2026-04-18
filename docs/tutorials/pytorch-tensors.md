# PyTorch tensors

[`TensorMessage`][cortex.messages.standard.TensorMessage] lets you pipe
tensors between processes with the same zero-copy multipart transport used
for NumPy arrays. Device and `requires_grad` metadata are preserved; the
bytes travel via the CPU side of the tensor.

## Publish

```python title="inference_producer.py"
import torch
import cortex
from cortex import Node, TensorMessage


class Inference(Node):
    def __init__(self):
        super().__init__("inference")
        self.pub = self.create_publisher("/model/features", TensorMessage)
        self.create_timer(1 / 30, self.tick)

    async def tick(self):
        # Fake feature tensor; could be output of a real model
        feats = torch.randn(4, 256, 7, 7, device="cuda" if torch.cuda.is_available() else "cpu")
        self.pub.publish(TensorMessage(data=feats, name="layer4_feats"))


cortex.run(Inference().run())
```

## Subscribe

```python title="downstream_consumer.py"
import cortex
from cortex import Node, TensorMessage
from cortex.messages.base import MessageHeader


async def on_features(msg: TensorMessage, header: MessageHeader):
    t = msg.data
    print(f"{msg.name}: shape={tuple(t.shape)} device={t.device} grad={t.requires_grad}")


class Consumer(Node):
    def __init__(self):
        super().__init__("consumer")
        self.create_subscriber("/model/features", TensorMessage, callback=on_features)


cortex.run(Consumer().run())
```

## What gets preserved

```mermaid
flowchart LR
    A[torch.Tensor<br/>cuda:0, grad=True] --> B[encode: .detach.cpu.numpy<br/>contiguous]
    B --> C[OOB frame + metadata<br/>device_str, requires_grad, dtype, shape]
    C -. IPC .-> D[decode: np.frombuffer<br/>torch.from_numpy]
    D --> E{cuda available?}
    E -- yes --> F[move to device_str]
    E -- no --> G[stay on CPU]
    F --> H[requires_grad_ True if flagged]
    G --> H
```

| Attribute            | Transported              |
| -------------------- | ------------------------ |
| `dtype`              | ✓ exact                  |
| `shape`              | ✓                        |
| `device`             | ✓ string; restored on decode if available |
| `requires_grad`      | ✓                        |
| `grad` (the actual gradient) | ✗ not sent       |
| autograd graph       | ✗ not sent (`detach()` is implicit) |

## Multi-tensor payloads

When you need several tensors together — e.g. a model's inputs and outputs
— use [`MultiTensorMessage`][cortex.messages.standard.MultiTensorMessage]:

```python
from cortex.messages.standard import MultiTensorMessage

msg = MultiTensorMessage(tensors={
    "image": image_tensor,
    "features": feat_tensor,
    "logits": logit_tensor,
})
pub.publish(msg)
```

Each tensor gets its own OOB frame; no bytes are copied into a container.

## Caveats

!!! warning "CPU detour is mandatory"
    Even for two processes on the same GPU, tensors are DMA'd to CPU on send
    and back to GPU on receive. That is a copy on each side. Cortex does not
    currently support CUDA IPC — for tight in-process handoffs, prefer a
    torch.multiprocessing queue or shared CUDA memory.

!!! note "Install with the `torch` extra"
    `TensorMessage` raises on construction if PyTorch is not installed. Use
    `pip install -e ".[torch]"`.

## See also

- [Concepts → Message wire format](../concepts/message-wire-format.md)
- [Components → Serialization](../components/serialization.md)
- [Tutorials → NumPy arrays & images](numpy-and-images.md)
