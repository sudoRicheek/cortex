# Serialization

> **Source:** [`cortex.utils.serialization`](../reference/utils/serialization.md),
> [`cortex.utils.hashing`](../reference/utils/hashing.md)

Two encodings live side by side: a **multipart / out-of-band** path that the
transport actually uses, and a **single-blob** path kept for the legacy
`Message.to_bytes` / `decode` API and tests. Both support the same Python
types; only their frame layout differs.

## Supported types

| Type                          | Inline path (`to_bytes`)      | OOB path (`to_frames`)        |
| ----------------------------- | ----------------------------- | ----------------------------- |
| `None`                        | 1 byte tag                    | msgpack `nil`                 |
| `int`, `float`, `str`, `bool` | msgpack PRIMITIVE             | msgpack                       |
| `bytes`                       | tag + length + bytes          | msgpack bin                   |
| `list`, `tuple`, `dict`       | msgpack with ExtType arrays   | msgpack with OOB descriptors  |
| `np.ndarray`                  | ExtType (inline bytes)        | OOB descriptor + extra frame  |
| `torch.Tensor`                | ExtType (inline bytes)        | OOB descriptor + extra frame  |

## The two paths, side by side

=== "OOB multipart (used on the wire)"

    ```mermaid
    flowchart LR
        V[values] --> E[_encode_transport_value]
        E --> Meta[msgpack metadata<br/>OOB descriptors for arrays]
        E --> Bufs[[buffer 0]]
        E --> Bufs2[[buffer 1]]
        Meta --> Out[(list of frames)]
        Bufs --> Out
        Bufs2 --> Out
    ```

    The function of interest is
    [`serialize_message_frames`][cortex.utils.serialization.serialize_message_frames]:

    ```python
    metadata_bytes, [buf0, buf1, ...] = serialize_message_frames(values)
    ```

    Arrays stay contiguous; ZMQ hands the buffer straight to the kernel.

=== "Inline blob (legacy / `Message.decode`)"

    ```mermaid
    flowchart LR
        V[values] --> P[msgpack.packb<br/>default=_msgpack_default]
        P --> Ext[ExtType 1/2 for arrays/tensors<br/>bytes embedded]
        Ext --> Blob[single bytes blob]
    ```

    The single blob round-trips through `serialize(value)` →
    `deserialize(data)`. Useful for persisting to disk, caches, or when you
    need a self-contained payload without tracking extra buffers.

## OOB descriptors

An out-of-band descriptor is a small dict that takes the place of the array
inside the msgpack metadata:

```python
# numpy
{"__cortex_oob__": "numpy", "buffer": 0, "dtype": "<f4", "shape": [480, 640, 3]}

# torch
{"__cortex_oob__": "torch", "buffer": 1, "dtype": "<f4",
 "shape": [1, 3, 224, 224], "device": "cuda:0", "requires_grad": True}
```

The `buffer` index refers into the ZMQ frames that follow the metadata.
Nested structures (dict of arrays, list of tensors, etc.) are walked
recursively by `_encode_transport_value` / `_decode_transport_value`.

## Zero-copy on the decode side

```mermaid
sequenceDiagram
    participant Sub as Subscriber
    participant ZMQ as zmq.Frame
    participant MV as memoryview
    participant NP as np.ndarray

    Sub->>ZMQ: recv_multipart(copy=False)
    ZMQ-->>Sub: frame with .buffer property
    Sub->>MV: memoryview(frame.buffer)
    Sub->>NP: np.frombuffer(mv, dtype).reshape(shape)
    Note over NP: array aliases the ZMQ frame memory
```

!!! warning "Aliasing caveat"
    The returned NumPy array is **a view over the ZMQ frame buffer**. It is
    safe to read as long as the frame lives, which is at least until your
    callback returns. If you need to:

    - mutate the array, or
    - keep it past the callback,

    call `arr = arr.copy()` first. This is cheap compared to the savings on
    the hot path.

## PyTorch specifics

- Tensors are **always moved to CPU** for transport. Transport frames carry
  the tensor's CPU bytes plus the original device string.
- On decode, CUDA tensors are moved back to the original device when CUDA is
  available; otherwise they stay on CPU.
- `requires_grad` is preserved.

## Fingerprinting

Separate but related: [`compute_fingerprint(cls)`][cortex.utils.hashing.compute_fingerprint]
computes a 64-bit identity from the module path, class name, and sorted
`field:type` pairs. Cached per-class in `_fingerprint_cache`. See
[Concepts → Fingerprinting](../concepts/fingerprinting.md) for the full story.

## When to use each helper

| Helper                                                                                            | Use when                                                   |
| ------------------------------------------------------------------------------------------------- | ---------------------------------------------------------- |
| [`serialize_message_frames`][cortex.utils.serialization.serialize_message_frames]                 | You're building a custom transport that speaks multipart   |
| [`deserialize_message_frames`][cortex.utils.serialization.deserialize_message_frames]             | Decoding the above                                         |
| [`serialize(value)`][cortex.utils.serialization.serialize] / [`deserialize`][cortex.utils.serialization.deserialize] | Persisting a single value to disk / cache                  |
| [`serialize_numpy`][cortex.utils.serialization.serialize_numpy] / [`deserialize_numpy`][cortex.utils.serialization.deserialize_numpy] | Raw array round-trip without msgpack overhead              |
| `Message.to_frames` / `Message.from_frames`                                                        | Anything inside Cortex itself                              |

## See also

- [Concepts → Message wire format](../concepts/message-wire-format.md)
- [Concepts → Fingerprinting](../concepts/fingerprinting.md)
- [Guides → Performance tuning](../guides/performance-tuning.md)
