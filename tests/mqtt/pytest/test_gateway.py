# SPDX-FileCopyrightText: Benjamin Cabé <kartben@gmail.com>
# SPDX-License-Identifier: GPL-3.0

"""What the gateway puts on, and takes off, a real MQTT broker."""

from conftest import MQTT_ROOT

OUR_NODE_ID = 0x12345678
PEER_NODE_ID = 0x87654321
BROADCAST = 0xFFFFFFFF

OUR_GATEWAY_ID = "!12345678"
OTHER_GATEWAY_ID = "!aabbccdd"

CHANNEL = "LongFast"
UPLINK_TOPIC = f"{MQTT_ROOT}/2/e/{CHANNEL}/{OUR_GATEWAY_ID}"
DOWNLINK_TOPIC = f"{MQTT_ROOT}/2/e/{CHANNEL}/{OTHER_GATEWAY_ID}"

# The hash every LongFast node running the default key puts on air.
LONGFAST_CHANNEL_HASH = 8

# Bytes of Meshtastic wire header in front of the encrypted payload.
HEADER_LEN = 16


def encrypted_envelope(pb, sender, packet_id, *, channel=CHANNEL, gateway=OTHER_GATEWAY_ID):
    """An encrypted broadcast, shaped the way another gateway would publish it."""
    service_envelope = pb.mqtt.ServiceEnvelope()
    service_envelope.channel_id = channel
    service_envelope.gateway_id = gateway

    packet = service_envelope.packet
    setattr(packet, "from", sender)
    packet.to = BROADCAST
    packet.id = packet_id
    packet.channel = LONGFAST_CHANNEL_HASH
    packet.encrypted = bytes(range(16))

    return service_envelope.SerializeToString()


def cleartext_envelope(pb, sender, packet_id):
    service_envelope = pb.mqtt.ServiceEnvelope()
    service_envelope.channel_id = CHANNEL
    service_envelope.gateway_id = OTHER_GATEWAY_ID

    packet = service_envelope.packet
    setattr(packet, "from", sender)
    packet.to = BROADCAST
    packet.id = packet_id
    packet.decoded.portnum = pb.portnums.PortNum.TEXT_MESSAGE_APP
    packet.decoded.payload = b"plain"

    return service_envelope.SerializeToString()


def test_the_gateway_subscribes_to_the_downlink_topic_of_its_channel(gateway):
    assert gateway.subscribe_topic == f"{MQTT_ROOT}/2/e/{CHANNEL}/+"


def test_a_heard_packet_is_uplinked_as_a_service_envelope(gateway, observer, pb):
    wire = gateway.hear(PEER_NODE_ID, 0x0BEEF001, "uplink")

    message = observer.next_message()
    assert message.topic == UPLINK_TOPIC

    received = pb.mqtt.ServiceEnvelope()
    received.ParseFromString(message.payload)

    assert received.channel_id == CHANNEL
    assert received.gateway_id == OUR_GATEWAY_ID
    assert getattr(received.packet, "from") == PEER_NODE_ID
    assert received.packet.to == BROADCAST
    assert received.packet.id == 0x0BEEF001
    assert received.packet.hop_start == 3
    assert received.packet.channel == LONGFAST_CHANNEL_HASH
    assert received.packet.WhichOneof("payload_variant") == "encrypted"
    assert received.packet.encrypted == wire[HEADER_LEN:], "the payload is passed on untouched"


def test_our_own_transmission_is_uplinked_too(gateway, observer, pb):
    gateway.send("mine")

    received = pb.mqtt.ServiceEnvelope()
    received.ParseFromString(observer.next_message().payload)

    assert getattr(received.packet, "from") == OUR_NODE_ID


def test_uplink_is_skipped_when_the_channel_disables_it(gateway, observer):
    gateway.set_uplink(False)

    gateway.hear(PEER_NODE_ID, 0x0BEEF002, "quiet")

    observer.expect_silence()


def test_a_downlink_envelope_reaches_the_mesh(gateway, observer, pb):
    observer.publish(DOWNLINK_TOPIC, encrypted_envelope(pb, PEER_NODE_ID, 0x0BEEF003))

    assert gateway.wait_for_transmit() == 1


def test_a_downlink_is_not_uplinked_back_to_the_broker(gateway, observer, pb):
    observer.publish(DOWNLINK_TOPIC, encrypted_envelope(pb, PEER_NODE_ID, 0x0BEEF004))

    assert gateway.wait_for_transmit() == 1
    observer.expect_silence()


def test_a_downlink_from_our_own_gateway_is_ignored(gateway, observer, pb):
    observer.publish(
        DOWNLINK_TOPIC, encrypted_envelope(pb, PEER_NODE_ID, 0x0BEEF005, gateway=OUR_GATEWAY_ID)
    )

    gateway.expect_no_transmit()


def test_a_downlink_we_originally_sent_is_ignored(gateway, observer, pb):
    observer.publish(DOWNLINK_TOPIC, encrypted_envelope(pb, OUR_NODE_ID, 0x0BEEF006))

    gateway.expect_no_transmit()


def test_a_downlink_for_an_unknown_channel_is_ignored(gateway, observer, pb):
    observer.publish(
        DOWNLINK_TOPIC, encrypted_envelope(pb, PEER_NODE_ID, 0x0BEEF007, channel="SomeOtherChannel")
    )

    gateway.expect_no_transmit()


def test_a_downlink_is_ignored_when_the_channel_disables_it(gateway, observer, pb):
    gateway.set_downlink(False)

    observer.publish(DOWNLINK_TOPIC, encrypted_envelope(pb, PEER_NODE_ID, 0x0BEEF008))

    gateway.expect_no_transmit()


def test_a_cleartext_downlink_is_ignored_while_encryption_is_on(gateway, observer, pb):
    observer.publish(DOWNLINK_TOPIC, cleartext_envelope(pb, PEER_NODE_ID, 0x0BEEF009))

    gateway.expect_no_transmit()
