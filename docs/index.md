# Cortex

**A lightweight Python framework for inter-process communication over ZeroMQ.**

Cortex is a pub/sub layer designed to feel obvious. Nodes publish typed messages on named topics; subscribers receive them via async callbacks. A tiny discovery daemon tells subscribers where to connect. Native support for NumPy arrays and PyTorch tensors keeps robotics- and ML-shaped payloads fast.

<div class="grid cards" markdown>

-   :material-rocket-launch-outline: **[Getting started](getting-started/quickstart.md)**

    Install, start the daemon, publish your first message in under two minutes.

-   :material-book-open-variant: **[Concepts](concepts/architecture.md)**

    How the wire format, fingerprinting, discovery handshake, and async execution fit together.

-   :material-puzzle-outline: **[Components](components/messages.md)**

    Deep dives into the Messages, Discovery, and Core modules.

-   :material-api: **[API reference](reference/index.md)**

    Auto-generated from the source. Always matches the code on `main`.

</div>

## Highlights

- **Publisher / Subscriber pattern** over ZeroMQ PUB/SUB sockets.
- **Discovery service** for automatic topic → endpoint resolution.
- **IPC transport** with zero-copy frames for large NumPy / PyTorch payloads.
- **64-bit fingerprint hashing** for fast message-type identification.
- **uvloop-backed async** on Linux/macOS for lower tail latency.

## Minimal example

=== "Publisher"

    ```python
    import numpy as np
    import cortex
    from cortex import Node, ArrayMessage


    class Cam(Node):
        def __init__(self):
            super().__init__("cam")
            self.pub = self.create_publisher("/cam/frame", ArrayMessage)
            self.create_timer(1 / 30, self.tick)

        async def tick(self):
            self.pub.publish(ArrayMessage(data=np.random.randn(480, 640).astype("f4")))


    cortex.run(Cam().run())
    ```

=== "Subscriber"

    ```python
    import cortex
    from cortex import Node, ArrayMessage
    from cortex.messages.base import MessageHeader


    async def on_frame(msg: ArrayMessage, header: MessageHeader):
        print(f"seq={header.sequence} shape={msg.data.shape}")


    class Viewer(Node):
        def __init__(self):
            super().__init__("viewer")
            self.create_subscriber("/cam/frame", ArrayMessage, callback=on_frame)


    cortex.run(Viewer().run())
    ```

## Project status

Cortex targets single-host process graphs today. See [design-review.md](design-review.md)
and [critique.md](critique.md) for an honest account of current limits and the
roadmap toward multi-host robotics use.
