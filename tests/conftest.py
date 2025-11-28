"""
Pytest fixtures for Cortex tests.
"""

import contextlib
import os
import threading
import time
from collections.abc import Generator

import pytest
import zmq


@pytest.fixture
def zmq_context() -> Generator[zmq.Context, None, None]:
    """Provide a ZMQ context for tests."""
    ctx = zmq.Context()
    yield ctx
    ctx.term()


@pytest.fixture
def temp_ipc_address(tmp_path) -> str:
    """Provide a temporary IPC address."""
    return f"ipc://{tmp_path}/test_socket"


@pytest.fixture
def discovery_address(tmp_path) -> str:
    """Provide a discovery daemon address."""
    return f"ipc://{tmp_path}/cortex_discovery"


@pytest.fixture
def cleanup_cortex_temp():
    """Clean up Cortex temporary files after tests."""
    yield
    # Cleanup
    import shutil

    cortex_dir = "/tmp/cortex"
    if os.path.exists(cortex_dir):
        with contextlib.suppress(Exception):
            shutil.rmtree(cortex_dir)


class DiscoveryDaemonFixture:
    """
    Test fixture for running a discovery daemon in a background thread.
    """

    def __init__(self, address: str):
        self.address = address
        self._daemon = None
        self._thread = None
        self._started = threading.Event()

    def start(self) -> None:
        """Start the discovery daemon."""
        from cortex.discovery.daemon import DiscoveryDaemon

        self._daemon = DiscoveryDaemon(address=self.address)

        def run_daemon():
            self._started.set()
            with contextlib.suppress(Exception):
                self._daemon.start()  # Daemon was stopped

        self._thread = threading.Thread(target=run_daemon, daemon=True)
        self._thread.start()

        # Wait for daemon to start
        self._started.wait(timeout=5.0)
        time.sleep(0.1)  # Extra time for socket binding

    def stop(self) -> None:
        """Stop the discovery daemon."""
        if self._daemon:
            # Signal shutdown first
            self._daemon._running = False
            self._daemon._shutdown_event.set()

            # Close socket (LINGER is already set on creation)
            if self._daemon._socket:
                with contextlib.suppress(Exception):
                    self._daemon._socket.close()
                self._daemon._socket = None

        # Wait for thread to finish
        if self._thread:
            self._thread.join(timeout=2.0)
            self._thread = None

        # Terminate context
        if self._daemon and self._daemon._context:
            with contextlib.suppress(Exception):
                self._daemon._context.term()
            self._daemon._context = None

        self._daemon = None


@pytest.fixture
def discovery_daemon(
    discovery_address: str,
) -> Generator[DiscoveryDaemonFixture, None, None]:
    """Provide a running discovery daemon."""
    fixture = DiscoveryDaemonFixture(discovery_address)
    fixture.start()
    yield fixture
    fixture.stop()
