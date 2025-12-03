"""
Discovery protocol definitions for Cortex.

Defines the request/response messages for the discovery service.
"""

from dataclasses import dataclass
from enum import IntEnum

import msgpack


class DiscoveryCommand(IntEnum):
    """Commands for the discovery service."""

    REGISTER_TOPIC = 1
    UNREGISTER_TOPIC = 2
    LOOKUP_TOPIC = 3
    LIST_TOPICS = 4
    SHUTDOWN = 99


class DiscoveryStatus(IntEnum):
    """Status codes for discovery responses."""

    OK = 0
    NOT_FOUND = 1
    ALREADY_EXISTS = 2
    ERROR = 3


@dataclass
class TopicInfo:
    """Information about a registered topic."""

    name: str  # Topic name (e.g., "/camera/image")
    address: str  # ZMQ IPC address (e.g., "ipc:///tmp/cortex/topics/camera_image")
    message_type: str  # Message type name
    fingerprint: int  # 64-bit message fingerprint
    publisher_node: str  # Name of the publishing node

    def to_bytes(self) -> bytes:
        """Serialize topic info to bytes."""
        data = {
            "name": self.name,
            "address": self.address,
            "message_type": self.message_type,
            "fingerprint": self.fingerprint,
            "publisher_node": self.publisher_node,
        }
        return msgpack.packb(data, use_bin_type=True)

    @classmethod
    def from_bytes(cls, data: bytes) -> "TopicInfo":
        """Deserialize topic info from bytes."""
        d = msgpack.unpackb(data, raw=False)
        return cls(**d)


@dataclass
class DiscoveryRequest:
    """Request message for the discovery service."""

    command: DiscoveryCommand
    topic_info: TopicInfo | None = None  # For REGISTER/UNREGISTER
    topic_name: str | None = None  # For LOOKUP

    def to_bytes(self) -> bytes:
        """Serialize request to bytes."""
        data = {
            "command": int(self.command),
            "topic_name": self.topic_name,
        }
        if self.topic_info:
            data["topic_info"] = self.topic_info.to_bytes()
        return msgpack.packb(data, use_bin_type=True)

    @classmethod
    def from_bytes(cls, data: bytes) -> "DiscoveryRequest":
        """Deserialize request from bytes."""
        d = msgpack.unpackb(data, raw=False)
        topic_info = None
        if "topic_info" in d and d["topic_info"]:
            topic_info = TopicInfo.from_bytes(d["topic_info"])
        return cls(
            command=DiscoveryCommand(d["command"]),
            topic_info=topic_info,
            topic_name=d.get("topic_name"),
        )


@dataclass
class DiscoveryResponse:
    """Response message from the discovery service."""

    status: DiscoveryStatus
    message: str = ""
    topic_info: TopicInfo | None = None  # For LOOKUP
    topics: list[TopicInfo] | None = None  # For LIST_TOPICS

    def to_bytes(self) -> bytes:
        """Serialize response to bytes."""
        data = {
            "status": int(self.status),
            "message": self.message,
        }
        if self.topic_info:
            data["topic_info"] = self.topic_info.to_bytes()
        if self.topics:
            data["topics"] = [t.to_bytes() for t in self.topics]
        return msgpack.packb(data, use_bin_type=True)

    @classmethod
    def from_bytes(cls, data: bytes) -> "DiscoveryResponse":
        """Deserialize response from bytes."""
        d = msgpack.unpackb(data, raw=False)
        topic_info = None
        topics = None
        if "topic_info" in d and d["topic_info"]:
            topic_info = TopicInfo.from_bytes(d["topic_info"])
        if "topics" in d and d["topics"]:
            topics = [TopicInfo.from_bytes(t) for t in d["topics"]]
        return cls(
            status=DiscoveryStatus(d["status"]),
            message=d.get("message", ""),
            topic_info=topic_info,
            topics=topics,
        )
