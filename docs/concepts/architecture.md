# Architecture

Cortex has three moving parts: the **discovery daemon**, **publisher** nodes,
and **subscriber** nodes. They coordinate over ZeroMQ — a REQ/REP control plane
for discovery and a PUB/SUB data plane for messages.

## High-level view

```mermaid
flowchart TB
    subgraph CP[Control plane]
        DD[Discovery daemon<br/><small>ipc:///tmp/cortex/discovery.sock</small>]
    end

    subgraph DP[Data plane]
        direction LR
        P[Publisher node] -- "PUB / SUB (IPC)" --> S[Subscriber node]
    end

    P -- REGISTER --> DD
    S -- LOOKUP --> DD
    DD -- TopicInfo --> S

    classDef daemon fill:#6366f1,stroke:#312e81,color:#fff
    classDef node fill:#0ea5e9,stroke:#0369a1,color:#fff
    class DD daemon
    class P,S node
```

## Message journey

Tracing one frame end to end:

```mermaid
sequenceDiagram
    autonumber
    participant User as User code
    participant Pub as Publisher
    participant Sock as ZMQ PUB socket
    participant Net as IPC
    participant SSock as ZMQ SUB socket
    participant Sub as Subscriber
    participant CB as async callback

    User->>Pub: publish(Message)
    Pub->>Pub: build header (fingerprint, ts, seq)
    Pub->>Pub: encode field values + OOB buffers
    Pub->>Sock: send_multipart([topic, header, metadata, *buffers])
    Sock->>Net: zero-copy handoff
    Net->>SSock: frames delivered
    SSock->>Sub: recv_multipart(copy=False)
    Sub->>Sub: Message.from_frames(...)
    Sub->>CB: await callback(msg, header)
```

Key invariant: array buffers ride as **separate ZMQ frames**, not inline in the
metadata. See [Message wire format](message-wire-format.md).

## Process layout

```mermaid
flowchart LR
    subgraph P1[Process: sensor]
        N1[Node<br/>shared zmq.asyncio.Context]
        PUB1[Publisher /sensor/a]
        PUB2[Publisher /sensor/b]
        T1[Timer 30 Hz]
        N1 --> PUB1
        N1 --> PUB2
        N1 --> T1
    end

    subgraph P2[Process: processor]
        N2[Node]
        SUB1[Subscriber /sensor/a]
        SUB2[Subscriber /sensor/b]
        PUB3[Publisher /processed]
        N2 --> SUB1
        N2 --> SUB2
        N2 --> PUB3
    end

    PUB1 -.->|IPC| SUB1
    PUB2 -.->|IPC| SUB2
```

Each topic gets its own IPC socket under `/tmp/cortex/topics/`. A single `Node`
shares one `zmq.asyncio.Context` across all its publishers and subscribers to
avoid per-socket io thread overhead.

## See also

- [Message wire format](message-wire-format.md)
- [Fingerprinting](fingerprinting.md)
- [Discovery protocol](discovery-protocol.md)
- [Async execution model](async-execution-model.md)
