# SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
# SPDX-License-Identifier: GPL-3.0

"""Host side of the MQTT gateway test.

Starts a real mosquitto broker, watches it with an ordinary MQTT client, and
drives the firmware through its shell. The firmware reaches the broker over TCP
from native_sim, so the bytes on the wire are the ones a real gateway sends.
"""

from __future__ import annotations

import queue
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path
from types import SimpleNamespace

import paho.mqtt.client as mqtt
import pytest
from twister_harness import DeviceAdapter
from twister_harness.fixtures import get_ready_shell

BROKER_HOST = "127.0.0.1"
BROKER_PORT = 11883  # keep in sync with CONFIG_MESHTASTIC_MQTT_BROKER_PORT
MQTT_ROOT = "msh/EU_868"

PROTO_DIR = Path(__file__).resolve().parents[3] / "src" / "proto"


@pytest.fixture(scope="session")
def pb(tmp_path_factory: pytest.TempPathFactory) -> SimpleNamespace:
    """Protobuf bindings generated from the same files the firmware is built from."""
    out_dir = tmp_path_factory.mktemp("protobuf")
    protos = sorted(str(p.relative_to(PROTO_DIR)) for p in PROTO_DIR.rglob("*.proto"))

    subprocess.run(
        [
            sys.executable,
            "-m",
            "grpc_tools.protoc",
            f"-I{PROTO_DIR}",
            f"--python_out={out_dir}",
            *protos,
        ],
        check=True,
    )

    sys.path.insert(0, str(out_dir))
    from meshtastic import mqtt_pb2, portnums_pb2

    return SimpleNamespace(mqtt=mqtt_pb2, portnums=portnums_pb2)


def _port_is_open(port: int, timeout: float) -> bool:
    deadline = time.time() + timeout

    while time.time() < deadline:
        with socket.socket() as sock:
            sock.settimeout(0.5)
            if sock.connect_ex((BROKER_HOST, port)) == 0:
                return True
        time.sleep(0.1)

    return False


@pytest.fixture(scope="session")
def broker(tmp_path_factory: pytest.TempPathFactory):
    if shutil.which("mosquitto") is None:
        pytest.fail("mosquitto is not installed (apt-get install mosquitto)")

    if _port_is_open(BROKER_PORT, timeout=0.0):
        pytest.fail(f"something is already listening on port {BROKER_PORT}")

    config = tmp_path_factory.mktemp("mosquitto") / "mosquitto.conf"
    config.write_text(f"listener {BROKER_PORT} {BROKER_HOST}\nallow_anonymous true\n")

    process = subprocess.Popen(
        ["mosquitto", "-c", str(config)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.STDOUT,
    )

    if not _port_is_open(BROKER_PORT, timeout=10.0) or process.poll() is not None:
        process.terminate()
        pytest.fail(f"mosquitto never started listening on {BROKER_PORT}")

    yield

    process.terminate()
    process.wait(timeout=5)


class Observer:
    """An MQTT subscriber on the gateway's root topic."""

    def __init__(self, client: mqtt.Client):
        self._client = client
        self._messages: queue.Queue = queue.Queue()
        self._own_publishes: set = set()

    def on_message(self, client, userdata, message) -> None:
        # The broker echoes back what we publish ourselves, since we subscribe
        # to the whole root topic. Only the gateway's traffic is interesting.
        if (message.topic, bytes(message.payload)) not in self._own_publishes:
            self._messages.put(message)

    def forget_everything(self) -> None:
        while not self._messages.empty():
            self._messages.get_nowait()

    def next_message(self, timeout: float = 10.0):
        try:
            return self._messages.get(timeout=timeout)
        except queue.Empty:
            raise AssertionError("nothing was published to the broker") from None

    def expect_silence(self, seconds: float = 1.0) -> None:
        try:
            message = self._messages.get(timeout=seconds)
        except queue.Empty:
            return
        raise AssertionError(f"unexpected publish on {message.topic}")

    def publish(self, topic: str, payload: bytes) -> None:
        self._own_publishes.add((topic, bytes(payload)))
        self._client.publish(topic, payload).wait_for_publish(timeout=5)


@pytest.fixture(scope="session")
def observer(broker) -> Observer:
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="pytest-observer")
    watcher = Observer(client)

    client.on_message = watcher.on_message
    client.connect(BROKER_HOST, BROKER_PORT)
    client.subscribe(f"{MQTT_ROOT}/#")
    client.loop_start()

    yield watcher

    client.loop_stop()
    client.disconnect()


class Gateway:
    """The firmware under test, driven through its shell."""

    def __init__(self, shell, subscribe_topic: str):
        self._shell = shell
        self.subscribe_topic = subscribe_topic

    def hear(self, sender: int, packet_id: int, text: str) -> bytes:
        """Feed the radio a frame from a peer, and return the frame it sent up."""
        for line in self._shell.exec_command(f"gw hear {sender:08x} {packet_id:08x} {text}"):
            if line.startswith("heard "):
                return bytes.fromhex(line.split()[1])
        raise AssertionError("the shell did not echo the injected frame")

    def send(self, text: str) -> None:
        self._shell.exec_command(f"gw send {text}")

    def transmit_count(self) -> int:
        for line in self._shell.exec_command("gw tx"):
            if line.startswith("tx "):
                return int(line.split()[1])
        raise AssertionError("the shell did not report a transmit count")

    def wait_for_transmit(self, timeout: float = 5.0) -> int:
        deadline = time.time() + timeout

        while time.time() < deadline:
            count = self.transmit_count()
            if count > 0:
                return count
            time.sleep(0.1)

        raise AssertionError("nothing was transmitted on the radio")

    def expect_no_transmit(self, seconds: float = 1.0) -> None:
        time.sleep(seconds)
        assert self.transmit_count() == 0, "the radio transmitted unexpectedly"

    def set_uplink(self, enabled: bool) -> None:
        self._shell.exec_command(f"gw uplink {'on' if enabled else 'off'}")

    def set_downlink(self, enabled: bool) -> None:
        self._shell.exec_command(f"gw downlink {'on' if enabled else 'off'}")

    def reset(self) -> None:
        self._shell.exec_command("gw reset")


@pytest.fixture(scope="session")
def gateway(broker, dut: DeviceAdapter) -> Gateway:
    """The firmware, once it has reached the broker and subscribed."""
    lines = dut.readlines_until(regex=r"MQTT subscribed to \S+", timeout=30.0)
    topic = lines[-1].split()[-1]

    return Gateway(get_ready_shell(dut), topic)


@pytest.fixture(autouse=True)
def clean_slate(gateway: Gateway, observer: Observer):
    gateway.reset()
    observer.forget_everything()
