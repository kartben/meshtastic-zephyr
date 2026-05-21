Meshtastic Zephyr Module
########################

This repository packages Zephyr's Meshtastic mesh radio stack as an external
Zephyr module. It contains the Meshtastic implementation, public headers,
protobuf schemas used for nanopb generation, and the Meshtastic sample.

The module is intended to be used from a Zephyr west workspace.

Why Zephyr?
***********

Zephyr is a good fit for Meshtastic because it provides:

* **Portability** across many MCU families and boards, which makes it easier to
  bring the same Meshtastic stack to different hardware targets.
* **Modularity** through Kconfig, devicetree, and the Zephyr module system, so
  features such as BLE, shell, GNSS, telemetry, UART, and MQTT can be enabled
  independently.
* **Existing drivers and subsystems** for the hardware and connectivity pieces
  that Meshtastic depends on, including LoRa radios, Bluetooth, UART, sensors,
  and IP networking.

Capabilities
************

The Zephyr Meshtastic subsystem implements a wire-compatible subset of the
`Meshtastic <https://meshtastic.org>`_ LoRa mesh protocol on top of Zephyr's
raw LoRa driver API.

Key features include:

* Exchange text messages with Meshtastic-compatible radios on the LongFast
  channel.
* Send and receive raw application payloads through the public C API.
* Configure channels, device role, and rebroadcast policy at runtime.
* Connect phone or host tools through shell, BLE PhoneAPI, UART PhoneAPI, or
  MQTT gateway support.
* Advertise optional GNSS position, telemetry, NodeInfo, and NodeDB data.

Usage
*****

Add the module to your west manifest or a Zephyr submanifest:

.. code-block:: yaml

   manifest:
     projects:
       - name: meshtastic-zephyr
         url: https://github.com/kartben/meshtastic-zephyr.git
         revision: main
         path: modules/lib/meshtastic-zephyr

Then update the workspace:

.. code-block:: console

   west update

The module requires Zephyr's ``nanopb`` module to be present in the west
workspace. Applications also need a board with a LoRa transceiver supported by
Zephyr's ``lora`` driver API.

Optional interfaces
*******************

Shell
=====

Enable ``CONFIG_SHELL`` and ``CONFIG_MESHTASTIC_SHELL`` to test the node from a
Zephyr shell.
Useful commands include::

  meshtastic status
  meshtastic text send [-c <index>] [dest|broadcast] <message>
  meshtastic send-port <dest|broadcast> <port> <payload>
  meshtastic channel list|show|set|disable ...
  meshtastic device role|rebroadcast ...

Feature-specific commands for GNSS, metrics, environment, NodeInfo, and NodeDB
appear when their matching options are enabled.

BLE PhoneAPI
============

Enable ``CONFIG_MESHTASTIC_BLE`` to use the node from the
Meshtastic mobile app. After flashing the board, pair with the advertised
Zephyr Meshtastic node to inspect settings and send messages through the app.

UART PhoneAPI
=============

Enable ``CONFIG_MESHTASTIC_SERIAL`` to use Meshtastic host tools
over a UART. Select the UART with the ``zephyr,meshtastic-uart`` chosen node
and use a dedicated UART when possible so console, logging, or shell output do
not corrupt the PhoneAPI frame stream.

MQTT gateway
============

Enable ``CONFIG_MESHTASTIC_MQTT`` on a board with IPv4
networking to bridge mesh packets to a Meshtastic-compatible MQTT broker. The
default configuration targets the public broker; set
``CONFIG_MESHTASTIC_MQTT_ROOT`` to change the root topic.

See the official `Meshtastic MQTT integration documentation
<https://meshtastic.org/docs/software/integrations/mqtt/>`_ for broker
behavior, topic layout, and client examples.

Sample
******

Build the Meshtastic sample with:

.. code-block:: console

   west build -b <board> modules/lib/meshtastic-zephyr/samples/meshtastic

The sample initializes the default LongFast channel, prints received text
messages, and periodically broadcasts a greeting.

License
*******

Zephyr-originated code in this repository is licensed under Apache-2.0. The
copied Meshtastic protobuf schema snapshot is provided with its own license
notice in ``src/proto/LICENSE.meshtastic-protobufs``.
