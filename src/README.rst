.. _meshtastic_subsys:

Meshtastic subsystem
####################

Overview
********

The Zephyr Meshtastic subsystem implements a wire-compatible subset of the
`Meshtastic <https://meshtastic.org>`_ LoRa mesh protocol on top of Zephyr's
raw LoRa driver API. Optional features such as shell commands, BLE, UART
serial, GNSS, telemetry, and MQTT can be enabled independently with Kconfig.

Key features
************

* Exchange text messages with Meshtastic-compatible radios on the LongFast
  channel.
* Send and receive raw application payloads through the public C API.
* Configure channels, device role, and rebroadcast policy at runtime.
* Connect phone or host tools through shell, BLE PhoneAPI, UART PhoneAPI, or
  MQTT gateway support.
* Advertise optional GNSS position, telemetry, NodeInfo, and NodeDB data.

Shell
*****

Enable :kconfig:option:`CONFIG_SHELL` and
:kconfig:option:`CONFIG_MESHTASTIC_SHELL` to test the node from a Zephyr shell.

Useful commands include::

  meshtastic status
  meshtastic text send [-c <index>] [dest|broadcast] <message>
  meshtastic send-port <dest|broadcast> <port> <payload>
  meshtastic channel list|show|set|disable ...
  meshtastic device role|rebroadcast ...

Feature-specific commands for GNSS, metrics, environment, NodeInfo, and NodeDB
appear when their matching options are enabled.

BLE PhoneAPI
************

Enable :kconfig:option:`CONFIG_MESHTASTIC_BLE` to use the node from the
Meshtastic mobile app. Flash the board, open the app on your phone, scan for
Bluetooth devices, and pair with the advertised Zephyr Meshtastic node. You can
then view node details, edit supported settings, and send messages from the app
through the Zephyr device.

UART PhoneAPI
*************

Enable :kconfig:option:`CONFIG_MESHTASTIC_SERIAL` to use Meshtastic host tools
over a UART. Select the UART with the ``zephyr,meshtastic-uart`` chosen node,
connect it to your host, then point a Meshtastic serial client at that port to
inspect the node and send messages. Use a dedicated UART when possible:
console, logging, or shell output on the same UART can corrupt the phone-app
frame stream.

MQTT gateway
************

Enable :kconfig:option:`CONFIG_MESHTASTIC_MQTT` on a board with IPv4
networking to bridge mesh packets to a Meshtastic-compatible MQTT broker. Once
connected, use an MQTT client to subscribe to the configured root topic and
confirm that packets heard on LoRa are published. Defaults target the public
broker; configure the root topic with
:kconfig:option:`CONFIG_MESHTASTIC_MQTT_ROOT`.

See the official `Meshtastic MQTT integration documentation
<https://meshtastic.org/docs/software/integrations/mqtt/>`_ for public broker
behavior, topic layout, and client examples.
