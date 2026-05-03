"""
Tests for the discovery system.
"""

import threading
import time

from cortex.discovery.client import DiscoveryClient
from cortex.discovery.daemon import DiscoveryDaemon
from cortex.discovery.protocol import (
    DiscoveryCommand,
    DiscoveryRequest,
    DiscoveryResponse,
    DiscoveryStatus,
    TopicInfo,
)


class TestTopicInfo:
    """Tests for TopicInfo serialization."""

    def test_topic_info_roundtrip(self):
        """TopicInfo should serialize and deserialize."""
        info = TopicInfo(
            name="/camera/image",
            address="ipc:///tmp/test_socket",
            message_type="ImageMessage",
            fingerprint=0x123456789ABCDEF0,
            publisher_node="camera_node",
        )

        data = info.to_bytes()
        restored = TopicInfo.from_bytes(data)

        assert restored.name == info.name
        assert restored.address == info.address
        assert restored.message_type == info.message_type
        assert restored.fingerprint == info.fingerprint
        assert restored.publisher_node == info.publisher_node


class TestDiscoveryProtocol:
    """Tests for discovery protocol messages."""

    def test_request_roundtrip(self):
        """DiscoveryRequest should serialize correctly."""
        request = DiscoveryRequest(
            command=DiscoveryCommand.REGISTER_TOPIC,
            topic_info=TopicInfo(
                name="/test",
                address="ipc:///tmp/test",
                message_type="TestMsg",
                fingerprint=12345,
                publisher_node="test_node",
            ),
        )

        data = request.to_bytes()
        restored = DiscoveryRequest.from_bytes(data)

        assert restored.command == request.command
        assert restored.topic_info.name == request.topic_info.name

    def test_response_roundtrip(self):
        """DiscoveryResponse should serialize correctly."""
        response = DiscoveryResponse(
            status=DiscoveryStatus.OK,
            message="Success",
            topic_info=TopicInfo(
                name="/test",
                address="ipc:///tmp/test",
                message_type="TestMsg",
                fingerprint=12345,
                publisher_node="test_node",
            ),
        )

        data = response.to_bytes()
        restored = DiscoveryResponse.from_bytes(data)

        assert restored.status == response.status
        assert restored.message == response.message
        assert restored.topic_info.name == response.topic_info.name


class TestDiscoveryDaemon:
    """Tests for discovery daemon."""

    def test_daemon_starts_and_stops(self, discovery_address):
        """Daemon should start and stop cleanly."""
        daemon = DiscoveryDaemon(address=discovery_address)

        # Start in background thread
        thread = threading.Thread(target=daemon.start, daemon=True)
        thread.start()

        time.sleep(0.2)  # Let it start

        daemon.stop()
        thread.join(timeout=5.0)

        assert not thread.is_alive()


class TestDiscoveryClient:
    """Tests for discovery client."""

    def test_register_and_lookup(self, discovery_daemon, discovery_address):
        """Client should register and lookup topics."""
        client = DiscoveryClient(discovery_address=discovery_address)

        topic_info = TopicInfo(
            name="/test/topic",
            address="ipc:///tmp/test_topic",
            message_type="TestMessage",
            fingerprint=12345,
            publisher_node="test_node",
        )

        # Register
        success = client.register_topic(topic_info)
        assert success

        # Lookup
        found = client.lookup_topic("/test/topic")
        assert found is not None
        assert found.name == "/test/topic"
        assert found.address == "ipc:///tmp/test_topic"

        client.close()

    def test_ping_alive(self, discovery_daemon, discovery_address):
        """Ping should return True when the daemon is reachable."""
        client = DiscoveryClient(discovery_address=discovery_address)
        assert client.ping() is True
        client.close()

    def test_ping_dead(self):
        """Ping should return False when no daemon is listening."""
        client = DiscoveryClient(
            discovery_address="ipc:///tmp/cortex/discovery_no_daemon.sock",
            timeout_ms=200,
            retries=1,
        )
        assert client.ping() is False
        client.close()

    def test_lookup_nonexistent(self, discovery_daemon, discovery_address):
        """Lookup of nonexistent topic should return None."""
        client = DiscoveryClient(discovery_address=discovery_address)

        found = client.lookup_topic("/nonexistent/topic")
        assert found is None

        client.close()

    def test_list_topics(self, discovery_daemon, discovery_address):
        """Client should list all topics."""
        client = DiscoveryClient(discovery_address=discovery_address)

        # Register some topics
        for i in range(3):
            info = TopicInfo(
                name=f"/test/topic_{i}",
                address=f"ipc:///tmp/topic_{i}",
                message_type="TestMessage",
                fingerprint=i,
                publisher_node="test_node",
            )
            client.register_topic(info)

        # List
        topics = client.list_topics()
        assert len(topics) >= 3

        names = [t.name for t in topics]
        assert "/test/topic_0" in names
        assert "/test/topic_1" in names
        assert "/test/topic_2" in names

        client.close()

    def test_unregister(self, discovery_daemon, discovery_address):
        """Client should unregister topics."""
        client = DiscoveryClient(discovery_address=discovery_address)

        info = TopicInfo(
            name="/test/unregister",
            address="ipc:///tmp/unregister",
            message_type="TestMessage",
            fingerprint=99999,
            publisher_node="test_node",
        )

        # Register
        client.register_topic(info)
        assert client.lookup_topic("/test/unregister") is not None

        # Unregister
        client.unregister_topic("/test/unregister")

        # Should be gone
        assert client.lookup_topic("/test/unregister") is None

        client.close()

    def test_wait_for_topic(self, discovery_daemon, discovery_address):
        """Client should wait for topic to appear."""
        client = DiscoveryClient(discovery_address=discovery_address)

        # Register topic after delay
        def delayed_register():
            time.sleep(0.5)
            client2 = DiscoveryClient(discovery_address=discovery_address)
            info = TopicInfo(
                name="/delayed/topic",
                address="ipc:///tmp/delayed",
                message_type="TestMessage",
                fingerprint=888,
                publisher_node="delayed_node",
            )
            client2.register_topic(info)
            client2.close()

        thread = threading.Thread(target=delayed_register, daemon=True)
        thread.start()

        # Wait for topic
        start = time.time()
        found = client.wait_for_topic("/delayed/topic", timeout=5.0, poll_interval=0.1)
        elapsed = time.time() - start

        assert found is not None
        assert found.name == "/delayed/topic"
        assert elapsed >= 0.4  # Should have waited

        client.close()
