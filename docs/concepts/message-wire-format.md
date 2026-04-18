# Message wire format

Cortex uses **ZeroMQ multipart messages**. Each published message is a list of
frames rather than a single blob. That lets array payloads ride as raw
contiguous buffers — no copy into a Python `bytes`, no re-copy by ZMQ.

## Frames on the wire

```mermaid
flowchart LR
    F0["Frame 0<br/>topic bytes"] --> F1
    F1["Frame 1<br/>header (24B)<br/>fingerprint • ts_ns • seq"] --> F2
    F2["Frame 2<br/>msgpack metadata<br/>(ordered field values)"] --> F3
    F3["Frame 3..N<br/>raw array buffers<br/>(OOB, zero-copy)"]
```

| Frame   | Contents                     | Size         |
| ------- | ---------------------------- | ------------ |
| 0       | Topic name (UTF-8)           | variable     |
| 1       | [`MessageHeader`][cortex.messages.base.MessageHeader] | **24 bytes** (3 × u64, big-endian) |
| 2       | msgpack-packed ordered field values; arrays replaced by OOB descriptors | small        |
| 3..N    | `np.ndarray.tobytes()` / `tensor.numpy().tobytes()`, contiguous | payload-sized |

## Header layout

```
offset 0        8       16       24
       |fp u64 |ts u64 |seq u64 |
        big-endian throughout
```

- `fp` — 64-bit message fingerprint, computed from class name and field schema.
- `ts` — publisher wall-clock in nanoseconds (`time.time_ns()`).
- `seq` — per-process, per-message-type monotonic counter.

## Metadata (Frame 2)

Field values are packed **in declaration order** (not by name), so the receiver
reconstructs using the dataclass's cached field tuple. This removes per-message
field-name encoding.

Arrays and tensors appear in the metadata as small dict stand-ins called
**OOB descriptors**:

```json
{
  "__cortex_oob__": "numpy",
  "buffer": 0,
  "dtype": "<f4",
  "shape": [480, 640, 3]
}
```

The `buffer` index refers into Frames 3..N. The receiver reconstructs:

```python
np.frombuffer(frame.buffer, dtype=np.dtype(desc["dtype"])).reshape(desc["shape"])
```

No copy. The resulting array **aliases the ZMQ frame memory** — copy it if you
need ownership or mutability (see [Performance tuning](../guides/performance-tuning.md)).

## Full encode/decode flow

```mermaid
sequenceDiagram
    participant U as User
    participant M as Message.to_frames
    participant S as serialize_message_frames
    participant E as _encode_transport_value
    participant Z as ZMQ send_multipart

    U->>M: build header + collect field values
    M->>S: values in declaration order
    S->>E: for each value, walk nested dicts/lists
    E-->>S: scalar stays inline; array → OOB descriptor + buffer appended
    S-->>M: (metadata_bytes, [buf0, buf1, ...])
    M-->>Z: [topic, header, metadata, *buffers]
```

## The legacy single-blob path

`Message.to_bytes()` / `from_bytes()` / `Message.decode()` still exist. They
pack *everything* into one msgpack blob using `ExtType` for arrays. That path
is retained for tests and opportunistic use; the transport always uses the
multipart path above.

!!! warning "Mismatch trap"
    Bytes captured from the wire cannot be fed to `Message.decode()` — the wire
    format is multipart, not a single blob. Use `Message.from_frames(frames)`.

## See also

- [Fingerprinting](fingerprinting.md)
- [`cortex.utils.serialization`](../reference/utils/serialization.md) — encoding helpers
- [`cortex.messages.base`](../reference/messages/base.md) — `Message`, `MessageHeader`
