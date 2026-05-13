# Cortex

Lightweight Python pub/sub over ZeroMQ IPC. Typed messages, automatic topic discovery, zero-copy frames for NumPy and PyTorch.

<div class="grid cards" markdown>

-   :material-rocket-launch-outline: **[Getting started](getting-started/quickstart.md)**

    Install, start the daemon, publish your first message.

-   :material-book-open-variant: **[Concepts](concepts/architecture.md)**

    Wire format, fingerprinting, discovery handshake, async execution.

-   :material-puzzle-outline: **[Components](components/messages.md)**

    Messages, discovery, publisher/subscriber, node + executors.

-   :material-api: **[API reference](reference/index.md)**

    Auto-generated from source.

</div>

## What you get

- PUB/SUB over ZeroMQ IPC.
- A discovery daemon for topic → endpoint resolution.
- 64-bit message fingerprints; strict type matching.
- Zero-copy out-of-band frames for NumPy arrays and PyTorch tensors.
- Async (`asyncio` + `uvloop`) and synchronous subscriber modes.

## Minimal example

=== "Publisher"

    ```python
    import numpy as np
    import cortex
    from cortex import Node
    from cortex.messages.standard import ArrayMessage


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
    from cortex import Node
    from cortex.messages.base import MessageHeader
    from cortex.messages.standard import ArrayMessage


    async def on_frame(msg: ArrayMessage, header: MessageHeader):
        print(f"seq={header.sequence} shape={msg.data.shape}")


    class Viewer(Node):
        def __init__(self):
            super().__init__("viewer")
            self.create_subscriber("/cam/frame", ArrayMessage, callback=on_frame)


    cortex.run(Viewer().run())
    ```

Run `cortex-discovery` once in the background, then run both files.

## C++ implementation

A standalone C++ port of the wire format (header, msgpack metadata, OOB buffers, discovery client, pub/sub) lives in [`cortex_wire_cpp/`](https://github.com/sudoRicheek/cortex/tree/main/cortex_wire_cpp). It's pure CMake, no ROS dependency, and exists for native consumers that want sub-millisecond latency without Python in the loop. See the [`README`](https://github.com/sudoRicheek/cortex/blob/main/cortex_wire_cpp/README.md) for the layout and [`DOCS.md`](https://github.com/sudoRicheek/cortex/blob/main/cortex_wire_cpp/DOCS.md) for the API.

A ROS 2 bridge built on top is at [`ros2_bridge/`](https://github.com/sudoRicheek/cortex/tree/main/ros2_bridge).

## Scope

Cortex targets single-host process graphs. Multi-host is not supported today.
