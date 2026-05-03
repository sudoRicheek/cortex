"""
Tests for ThreadedSubscriber — the synchronous, OS-thread-backed
low-latency subscriber path.

These tests share the existing ``discovery_daemon`` fixture from
``conftest.py`` but spin up subscribers and publishers directly without
going through ``Node`` so we can isolate the threaded receive logic from
asyncio. There is one separate test (``test_node_mode_dispatch``) that
exercises the ``Node.create_subscriber(mode='sync')`` plumbing.
"""

import asyncio
import threading
import time
from dataclasses import dataclass

import pytest

from cortex.core.publisher import Publisher
from cortex.core.subscriber_base import MessageFingerprintError
from cortex.core.sync_subscriber import ThreadedSubscriber
from cortex.messages.base import Message


@dataclass
class CmdMessage(Message):
    """Tiny control-style payload."""

    seq: int
    value: float


@dataclass
class OtherMessage(Message):
    """A second type with a different fingerprint."""

    payload: str


def _wait_for(condition, timeout: float = 5.0, interval: float = 0.01) -> bool:
    """Spin until ``condition()`` is truthy or ``timeout`` elapses."""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if condition():
            return True
        time.sleep(interval)
    return False


# ---------------------------------------------------------------------------
# Construction-time contract checks (no network needed)
# ---------------------------------------------------------------------------


def test_rejects_async_callback(discovery_daemon, discovery_address):
    """Sync mode must refuse coroutine callbacks loudly at construction."""
    pub = Publisher(
        topic_name="/test/sync_async_cb",
        message_type=CmdMessage,
        node_name="pub_async_cb",
        discovery_address=discovery_address,
    )
    time.sleep(0.1)

    async def coro_cb(_msg, _hdr):
        pass

    try:
        with pytest.raises(TypeError, match="synchronous"):
            ThreadedSubscriber(
                topic_name="/test/sync_async_cb",
                message_type=CmdMessage,
                callback=coro_cb,
                discovery_address=discovery_address,
                topic_timeout=2.0,
            )
    finally:
        pub.close()


def test_topic_timeout_raises(discovery_daemon, discovery_address):
    """If the topic never registers, construction should raise TimeoutError."""
    with pytest.raises(TimeoutError):
        ThreadedSubscriber(
            topic_name="/test/sync_never_registered",
            message_type=CmdMessage,
            callback=lambda _m, _h: None,
            discovery_address=discovery_address,
            topic_timeout=0.5,
        )


def test_fingerprint_mismatch_is_fatal(discovery_daemon, discovery_address):
    """Sync mode is strict about types — register one type, subscribe to another."""
    pub = Publisher(
        topic_name="/test/sync_fp_mismatch",
        message_type=OtherMessage,
        node_name="pub_fp_mismatch",
        discovery_address=discovery_address,
    )
    time.sleep(0.2)

    try:
        with pytest.raises(MessageFingerprintError):
            ThreadedSubscriber(
                topic_name="/test/sync_fp_mismatch",
                message_type=CmdMessage,
                callback=lambda _m, _h: None,
                discovery_address=discovery_address,
                topic_timeout=2.0,
            )
    finally:
        pub.close()


# ---------------------------------------------------------------------------
# Receive path
# ---------------------------------------------------------------------------


def _warmup(pub: Publisher, sub: ThreadedSubscriber, timeout: float = 3.0) -> None:
    """Pump sentinel messages until the SUB-PUB handshake completes.

    ZMQ SUB has slow-joiner semantics; messages published before the
    subscriber's filter has reached the publisher are silently dropped.
    Tests use sequence numbers >= 100000 for the warmup so callers can
    skip them.
    """
    deadline = time.monotonic() + timeout
    seq = 100000
    initial = sub.receive_count
    while sub.receive_count == initial and time.monotonic() < deadline:
        pub.publish(CmdMessage(seq=seq, value=0.0))
        seq += 1
        time.sleep(0.01)
    if sub.receive_count == initial:
        raise RuntimeError("SUB-PUB handshake never completed")


def test_roundtrip(discovery_daemon, discovery_address):
    """End-to-end: sync subscriber receives messages from a sync publisher."""
    pub = Publisher(
        topic_name="/test/sync_roundtrip",
        message_type=CmdMessage,
        node_name="pub_roundtrip",
        discovery_address=discovery_address,
    )
    time.sleep(0.2)

    received: list[CmdMessage] = []
    received_lock = threading.Lock()

    def cb(msg, _hdr):
        with received_lock:
            received.append(msg)

    sub = ThreadedSubscriber(
        topic_name="/test/sync_roundtrip",
        message_type=CmdMessage,
        callback=cb,
        discovery_address=discovery_address,
        # Plenty of headroom so we don't lose intermediate messages on bursts.
        queue_size=64,
        topic_timeout=2.0,
    )
    sub.start()

    try:
        _warmup(pub, sub)
        baseline = len(received)
        for i in range(5):
            assert pub.publish(CmdMessage(seq=i, value=float(i)))
            time.sleep(0.05)

        assert _wait_for(lambda: len(received) - baseline >= 5, timeout=3.0), (
            f"only got {len(received) - baseline} of 5 after warmup"
        )

        # Filter out warmup sentinels (seq >= 100000)
        post_warmup = [m for m in received[baseline:] if m.seq < 100000]
        seqs = sorted(m.seq for m in post_warmup[:5])
        assert seqs == [0, 1, 2, 3, 4]
        assert sub.receive_count >= 5
    finally:
        sub.close()
        pub.close()


def test_clean_shutdown_when_idle(discovery_daemon, discovery_address):
    """No messages, no problem: stop() should join inside the timeout."""
    pub = Publisher(
        topic_name="/test/sync_idle",
        message_type=CmdMessage,
        node_name="pub_idle",
        discovery_address=discovery_address,
    )
    time.sleep(0.2)

    sub = ThreadedSubscriber(
        topic_name="/test/sync_idle",
        message_type=CmdMessage,
        callback=lambda _m, _h: None,
        discovery_address=discovery_address,
        topic_timeout=2.0,
    )
    sub.start()
    assert sub.running

    t0 = time.monotonic()
    sub.close()
    elapsed = time.monotonic() - t0
    # Poll timeout is 50ms + some slack; 1s is comfortable.
    assert elapsed < 1.0, f"shutdown took {elapsed:.2f}s"
    assert not sub.running

    pub.close()


def test_callback_exception_does_not_kill_thread(discovery_daemon, discovery_address):
    """A throwing callback should be logged but the receive loop must continue."""
    pub = Publisher(
        topic_name="/test/sync_throwing_cb",
        message_type=CmdMessage,
        node_name="pub_throwing",
        discovery_address=discovery_address,
    )
    time.sleep(0.2)

    seen: list[int] = []
    seen_lock = threading.Lock()

    def cb(msg, _hdr):
        with seen_lock:
            seen.append(msg.seq)
        if msg.seq == 0:
            raise RuntimeError("synthetic failure")

    sub = ThreadedSubscriber(
        topic_name="/test/sync_throwing_cb",
        message_type=CmdMessage,
        callback=cb,
        discovery_address=discovery_address,
        queue_size=64,
        topic_timeout=2.0,
    )
    sub.start()

    try:
        # Warmup using the same sentinel scheme — but the cb here only
        # raises on seq==0, so warmup sentinels (>=100000) are safe.
        _warmup(pub, sub)
        baseline = len(seen)

        for i in range(3):
            assert pub.publish(CmdMessage(seq=i, value=float(i)))
            time.sleep(0.05)

        assert _wait_for(lambda: len(seen) - baseline >= 3, timeout=3.0), (
            f"only got {len(seen) - baseline} of 3 after warmup"
        )
        assert sub.running, "receive thread died on user exception"
    finally:
        sub.close()
        pub.close()


def test_small_queue_drops_intermediate(discovery_daemon, discovery_address):
    """With ``queue_size=1`` and a slow callback, the publisher's burst is
    dropped on the receive side: the subscriber sees far fewer messages
    than were sent, and the dropped counter reflects the gap."""
    pub = Publisher(
        topic_name="/test/sync_small_queue",
        message_type=CmdMessage,
        node_name="pub_small_queue",
        discovery_address=discovery_address,
    )
    time.sleep(0.2)

    received: list[int] = []
    received_lock = threading.Lock()

    def cb(msg, _hdr):
        # Slow callback so the publisher can outrun the receiver.
        time.sleep(0.02)
        with received_lock:
            received.append(msg.seq)

    sub = ThreadedSubscriber(
        topic_name="/test/sync_small_queue",
        message_type=CmdMessage,
        callback=cb,
        discovery_address=discovery_address,
        queue_size=1,
        topic_timeout=2.0,
    )
    sub.start()

    try:
        _warmup(pub, sub)
        baseline_received = sub.receive_count

        # Burst 200 messages as fast as possible.
        for i in range(200):
            pub.publish(CmdMessage(seq=i, value=float(i)))
        time.sleep(2.0)  # give the slow callback time to drain what survives

        post = sub.receive_count - baseline_received
        # We should have seen *some* messages but far fewer than 200.
        assert post > 0
        assert post < 200, f"expected drops but got all {post} messages"
        # Drop counter should reflect the gap.
        assert sub.dropped_count > 0
    finally:
        sub.close()
        pub.close()


def test_node_spin_pure_sync(discovery_daemon, discovery_address):
    """Node with only sync work runs end-to-end without entering asyncio."""
    from cortex.core.node import Node

    received: list[int] = []
    received_lock = threading.Lock()

    def cb(msg, _hdr):
        with received_lock:
            received.append(msg.seq)

    def producer(stop, pub):
        seq = 0
        while not stop.is_set() and seq < 1000:
            pub.publish(CmdMessage(seq=seq, value=float(seq)))
            seq += 1
            time.sleep(0.001)

    pub_node = Node(name="pure_sync_pub", discovery_address=discovery_address)
    sub_node = Node(name="pure_sync_sub", discovery_address=discovery_address)

    try:
        pub = pub_node.create_publisher(
            topic_name="/test/spin_sync",
            message_type=CmdMessage,
            mode="sync",
        )
        time.sleep(0.1)

        sub_node.create_subscriber(
            topic_name="/test/spin_sync",
            message_type=CmdMessage,
            callback=cb,
            mode="sync",
            queue_size=64,
            topic_timeout=2.0,
        )

        # Spawn a producer on the publisher node and spin until it's done.
        pub_node.spawn_thread(producer, pub, name="producer")

        # Run the subscriber spin in another thread so we can also spin
        # the publisher — both need a thread to host their `spin()`.
        sub_thread = threading.Thread(
            target=lambda: sub_node.spin(timeout=3.0),
            name="sub-spin",
        )
        sub_thread.start()

        # Spin publisher node until producer finishes (stop_event is auto-set
        # once close_sync runs; we just give it time).
        pub_node.spin(timeout=2.0)

        sub_thread.join(timeout=2.0)
        assert sub_node._running is False
        assert pub_node._running is False
        assert len(received) > 100, f"only got {len(received)} messages"
    finally:
        sub_node.close_sync()
        pub_node.close_sync()


def test_node_spin_rejects_async_work(discovery_daemon, discovery_address):
    """spin() must refuse if the node has async timers/subscribers."""
    from cortex.core.node import Node

    node = Node(name="mixed_node", discovery_address=discovery_address)
    try:

        async def tick():
            pass

        node.create_timer(0.1, tick)
        with pytest.raises(RuntimeError, match="async"):
            node.spin()
    finally:
        # Have to use async close — node has an async timer
        asyncio.run(node.close())


def test_async_strict_fingerprint(discovery_daemon, discovery_address):
    """Async subscriber with strict_fingerprint=True must raise on mismatch."""
    from cortex.core.subscriber import Subscriber

    pub = Publisher(
        topic_name="/test/async_fp_strict",
        message_type=OtherMessage,
        node_name="pub_async_fp",
        discovery_address=discovery_address,
    )
    time.sleep(0.2)

    try:
        # Construction does the fingerprint check via _lookup_nonblocking.
        with pytest.raises(MessageFingerprintError):
            Subscriber(
                topic_name="/test/async_fp_strict",
                message_type=CmdMessage,
                callback=None,
                discovery_address=discovery_address,
                wait_for_topic=False,
                strict_fingerprint=True,
            )
    finally:
        pub.close()


# ---------------------------------------------------------------------------
# Node integration
# ---------------------------------------------------------------------------


def test_node_sync_publisher_mode(discovery_daemon, discovery_address):
    """``Node.create_publisher(mode='sync')`` returns a Publisher with its
    own zmq.Context, suitable for being driven from a non-asyncio thread."""
    import zmq

    from cortex.core.node import Node

    async def main() -> None:
        node = Node(name="sync_pub_node", discovery_address=discovery_address)
        try:
            pub = node.create_publisher(
                topic_name="/test/sync_publisher",
                message_type=CmdMessage,
                mode="sync",
            )
            # Sync mode should NOT have wrapped the node's async context.
            assert not isinstance(pub._context, zmq.asyncio.Context)
            assert pub.publish(CmdMessage(seq=0, value=0.0))
            assert pub._sequence == 1
        finally:
            await node.close()

    asyncio.run(main())


def test_node_mode_dispatch(discovery_daemon, discovery_address):
    """Node.create_subscriber(mode='sync') returns a ThreadedSubscriber and
    Node.run starts/stops its thread cleanly."""
    from cortex.core.node import Node

    async def main() -> None:
        node_pub = Node(name="pub_node", discovery_address=discovery_address)
        node_sub = Node(name="sub_node", discovery_address=discovery_address)

        try:
            pub = node_pub.create_publisher(
                topic_name="/test/node_sync",
                message_type=CmdMessage,
            )
            await asyncio.sleep(0.2)

            received: list[int] = []
            received_lock = threading.Lock()

            def cb(msg, _hdr):
                with received_lock:
                    received.append(msg.seq)

            sub = node_sub.create_subscriber(
                topic_name="/test/node_sync",
                message_type=CmdMessage,
                callback=cb,
                mode="sync",
                queue_size=64,
                topic_timeout=2.0,
            )
            assert isinstance(sub, ThreadedSubscriber)

            run_task = asyncio.create_task(node_sub.run())
            # Give the node a tick to start the worker thread.
            await asyncio.sleep(0.1)
            assert sub.running

            # Warmup: pump sentinels (seq >= 100000) until the SUB filter
            # propagates and we see at least one delivery.
            warmup_deadline = time.monotonic() + 3.0
            wseq = 100000
            while time.monotonic() < warmup_deadline:
                with received_lock:
                    if received:
                        break
                pub.publish(CmdMessage(seq=wseq, value=0.0))
                wseq += 1
                await asyncio.sleep(0.01)
            with received_lock:
                assert received, "SUB-PUB handshake never completed"
                baseline = len(received)

            for i in range(3):
                assert pub.publish(CmdMessage(seq=i, value=float(i)))
                await asyncio.sleep(0.05)

            for _ in range(60):
                with received_lock:
                    if len(received) - baseline >= 3:
                        break
                await asyncio.sleep(0.05)
            with received_lock:
                post = [s for s in received[baseline:] if s < 100000]
            assert sorted(post[:3]) == [0, 1, 2]

            node_sub.stop()
            run_task.cancel()
            with contextlib_suppress():
                await run_task
        finally:
            await node_pub.close()
            await node_sub.close()

    asyncio.run(main())


# Tiny helper to keep the asyncio test readable without a top-level import
# of contextlib.suppress (the rest of this file is sync).
class contextlib_suppress:
    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        return exc_type is asyncio.CancelledError
