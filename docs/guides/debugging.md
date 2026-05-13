# Debugging

## Subscriber hangs on startup

The daemon isn't running or the topic name is mistyped. `DiscoveryClient.wait_for_topic_async` polls every 500 ms until the topic appears or the timeout fires.

```bash
cortex-discovery --log-level DEBUG
```

Watch for `LOOKUP topic: /x -> NOT FOUND`.

## Publisher "works" but subscriber gets nothing

Classic ZMQ slow-joiner problem: PUB drops messages for which no SUB is connected yet. If the publisher starts first and publishes immediately, the first few messages are lost.

Fixes:

- Make the subscriber start first (or use `wait_for_topic=True`, the default).
- Have the publisher sleep briefly after bind before its first publish.

## Stale `/tmp/cortex/topics/*.sock`

If a publisher exits uncleanly, its IPC socket file remains. `Publisher._setup_socket` unlinks any existing file at the same path on the next bind — so restarting the publisher fixes it. Manual cleanup:

```bash
rm /tmp/cortex/topics/<stale-socket>.sock
```

## Daemon state doesn't survive restart

The registry is in-memory. Restarting the daemon wipes everything; publishers don't auto-re-register. Restart your publishers after restarting the daemon.

## Fingerprint mismatch

`Message type mismatch for /x: expected FooMessage, got BarMessage` — the topic was registered with a different class. Either rename the topic or align the classes. The async path logs and continues; use `strict_fingerprint=True` to raise instead.

## Debug logging

```python
import logging
logging.basicConfig(level=logging.DEBUG)
```

Useful loggers: `cortex.publisher`, `cortex.subscriber`, `cortex.node`, `cortex.discovery`, `cortex.discovery.client`.
