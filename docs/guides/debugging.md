# Debugging

## Subscriber hangs on startup

Most likely: the daemon is not running, or the topic name is mistyped.
`DiscoveryClient.wait_for_topic_async` polls every 500 ms until the topic
appears or the timeout fires.

```bash
cortex-discovery --log-level DEBUG
```

Watch for `LOOKUP topic: /x -> NOT FOUND`.

## Publisher "works" but subscriber receives nothing

ZMQ PUB drops messages for which no matching SUB is connected yet. If your
publisher starts first and publishes immediately, the first few messages are
lost — this is the classic ZMQ slow-joiner problem.

Workarounds:

- Have the publisher wait briefly after bind before publishing the first message.
- Have the subscriber wait-for-topic (the default) so it comes up after the
  publisher registered.

## Stale `/tmp/cortex/topics/*.sock` files

If a publisher exits uncleanly, its IPC socket file remains. Cortex's
`Publisher._setup_socket` unlinks any existing file at the same path on the
**next bind** — so restarting the publisher fixes it. Otherwise:

```bash
rm /tmp/cortex/topics/<stale-socket>.sock
```

## Daemon state survives restarts — but doesn't

The registry is **in-memory**. Restarting the daemon wipes all state;
publishers do not auto-re-register today. Restart your publishers after
restarting the daemon.

## Fingerprint mismatch warning

If you see
`Message type mismatch for /x: expected FooMessage, got BarMessage` —
the topic was registered with a different message class. Either rename the
topic or align the classes.

## Debug logging

```python
import logging
logging.basicConfig(level=logging.DEBUG)
```

Cortex uses standard `logging`. Interesting loggers: `cortex.publisher`,
`cortex.subscriber`, `cortex.node`, `cortex.discovery`, `cortex.discovery.client`.
