Meshtastic Zephyr Module
########################

This repository packages Zephyr's Meshtastic mesh radio stack as an external
Zephyr module. It contains the Meshtastic implementation, public headers,
protobuf schemas used for nanopb generation, and the Meshtastic sample.

The module is intended to be used from a Zephyr west workspace.

Why Zephyr
**********

This project uses Zephyr because it provides:

* **Portability** across many MCU and radio platforms through a consistent RTOS,
  devicetree model, and build system.
* **Modularity** with Kconfig-controlled features so shell, BLE, UART, GNSS,
  telemetry, and MQTT capabilities can be enabled independently.
* **Existing drivers and subsystems**, especially Zephyr's LoRa, Bluetooth,
  networking, logging, and hardware abstraction layers, reducing custom
  platform code and integration effort.

Meshtastic subsystem
********************

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

Optional integrations:

* **Shell** (``CONFIG_SHELL`` + ``CONFIG_MESHTASTIC_SHELL``): local inspection
  and control commands such as ``meshtastic status`` and ``meshtastic text send``.
* **BLE PhoneAPI** (``CONFIG_MESHTASTIC_BLE``): connect from the Meshtastic
  mobile app.
* **UART PhoneAPI** (``CONFIG_MESHTASTIC_SERIAL``): connect host tools over a
  selected UART (prefer a dedicated UART to avoid stream corruption).
* **MQTT gateway** (``CONFIG_MESHTASTIC_MQTT``): bridge mesh traffic to a
  Meshtastic-compatible MQTT broker (see the `official Meshtastic MQTT docs
  <https://meshtastic.org/docs/software/integrations/mqtt/>`_).

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
