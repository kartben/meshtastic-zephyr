Zephyr-native Meshtastic stack
##############################

This repository packages a Zephyr native Meshtastic® stack that should work out-of-the-box on any
Zephyr-supported board + LoRa transceiver.

**DISCLAIMER**: this project is not affiliated with or endorsed by Meshtastic LLC. Meshtastic® is a
registered trademark of Meshtastic LLC.

Why Zephyr?
***********

The `original Meshtastic firmware <https://github.com/meshtastic/firmware>`_ is a PlatformIO/Arduino
application with a (very!) large amount of device-specific integration code (including drivers for
sensors, displays, etc.) and lots of places in the code where conditional compilation is used to
adapt to different hardware platforms.

Zephyr RTOS _is_ designed to abstract away the hardware details and already provides ready-to-use
subsystems for many of the features that Meshtastic needs (LoRa, Bluetooth, GNSS, ...).

Feature support
***************

The initial goal is to provide a minimal set of API for setting up and configuring the stack, as
wel as sending and receiving messages.

A typical "original" Meshtastic firmware would typically expose a UI but this initial part doesn't
have any at this point and instead focus has been put on providing shell commands that can be
executed directly from the device's console (e.g. ``meshtastic status``, ``meshtastic nodedb list``,
...), and on providing a PhoneAPI interface over Bluetooth LE and serial so that a host application
(see https://meshtastic.org/downloads/) can provide a nice frontend to the device.

The original Meshtastic firmware is inherently very re-configurable (e.g. LoRa radio settings, node
identification, ...) without having to recompile it (i.e. change your LoRa presets, reboot device,
done!). This project, however, is currently mainly proposing *build-time* configuration, (using
Kconfig) so trying to reconfigure it using your favorite app's config UI will simply not work (and
likely fail miserably). Support for settings (persisted in flash) has started to be introduced
though, and configuring channels (tested through shell commands only at the time of writing) works
as expected and channels survive a reboot.

As this project is mostly a rewrite from scratch, and as there aren't really (to the best of my
knowledge) any interoperability test suites available, the support matrix below reflects what has
been implemented and tested against the reference
`Meshtastic firmware <https://github.com/meshtastic/firmware>`_ and the official
`module documentation <https://meshtastic.org/docs/configuration/module/>`_.

Legend: ✅ Full · 🟡 Partial · ❌ Not yet

.. list-table::
   :header-rows: 1
   :widths: 30 12 58

   * - Feature
     - Status
     - Notes
   * - **Core mesh**
     -
     -
   * - Packet routing / flood relay
     - ✅
     - Wire-compatible encode/decode, hop-limit rebroadcast
   * - Channel encryption
     - ✅
     - AES-CTR, per-channel PSK (PSA crypto backend)
   * - Duplicate suppression
     - ✅
     - Configurable packet dedup cache
   * - **Phone interface**
     -
     -
   * - phoneAPI (ToRadio / FromRadio)
     - ✅
     - ToRadio / FromRadio protobuf protocol
   * - Bluetooth LE transport
     - ✅
     - GATT service, compatible with the Meshtastic apps (tested on Android, macOS, and web client)
   * - Serial / UART transport
     - ✅
     - StreamAPI framing over a chosen UART (test on Android, macOS, and web client)
   * - Settings edit over phoneAPI
     - ❌
     - Read/handshake only — use the shell to configure
   * - **Messaging & telemetry**
     -
     -
   * - Text messages
     - ✅
     - Send / receive UTF-8 plaintext
   * - Position / GNSS
     - ✅
     - Integration with Zephyr GNSS subsystem, periodic position broadcast
   * - NodeInfo
     - ✅
     - Periodic identity broadcast + node discovery
   * - NodeDB
     - 🟡
     - In-RAM node cache only (not persisted to flash)
   * - Device telemetry
     - ✅
     - Uptime, optional battery / fuel-gauge metrics, airtime/channel utilization metrics
   * - Environment telemetry
     - ✅
     - Temperature, humidity, pressure, gas, light sensors.
       Automatically picks up default sensors (e.g. ``ambient-temp0`` from Devicetree).
   * - MQTT
     - 🟡
     - Gateway uplink/downlink bridge ; no per-channel configuration for now
   * - **Configuration**
     -
     -
   * - Local config persistence
     - 🟡
     - Zephyr settings subsystem backed by NVS flash (channels configuration only)

Shell commands
**************

When ``CONFIG_MESHTASTIC_SHELL`` is enabled, the firmware registers a ``meshtastic`` command tree on
the Zephyr . Commands marked *(optional)* are only present when their corresponding
``CONFIG_MESHTASTIC_*`` Kconfig option is set.

.. list-table::
   :header-rows: 1
   :widths: 42 58

   * - Command
     - Description
   * - ``meshtastic status``
     - Show node ID, init/BLE state, TX/RX stats, channel, role
   * - ``meshtastic channel list``
     - List the 8 channel slots
   * - ``meshtastic channel show <index>``
     - Show one channel slot
   * - ``meshtastic channel set <index> [name|role|psk|uplink|downlink]...``
     - Update fields of a channel slot
   * - ``meshtastic channel disable <index>``
     - Disable a channel slot
   * - ``meshtastic device role [name]``
     - Get / set the device role
   * - ``meshtastic device rebroadcast [mode]``
     - Get / set the rebroadcast mode
   * - ``meshtastic text send [-c <index>] [dest|broadcast] <message>``
     - Send a text message *(optional)*
   * - ``meshtastic nodedb list``
     - List known nodes *(optional)*
   * - ``meshtastic nodedb show <node|0xnode>``
     - Show one NodeDB entry *(optional)*
   * - ``meshtastic gnss send [dest|broadcast]``
     - Send the current GNSS position *(optional)*
   * - ``meshtastic metrics send [dest|broadcast]``
     - Send device telemetry *(optional)*
   * - ``meshtastic environment send [dest|broadcast]``
     - Send environment telemetry *(optional)*
   * - ``meshtastic nodeinfo send [dest|broadcast]``
     - Send node information *(optional)*

Setup
*****

This repository can be used to directly provision a full-blown workspace from scratch, or it can be
pulled into an existing workspace as a module.

As a standalone workspace
-------------------------

Before getting started, make sure you have followed the initial steps of the Zephyr Getting Started
Guide up to install/update host dependencies that Zephyr requires such as Python, CMake, ...

At the "Get the Zephyr source code" step, replace the default ``west init <workspace_location>`` step
with the following command so that you use this repository as the main manifest for your workspace:

.. code-block:: console

   $ west init -m https://github.com/kartben/meshtastic-zephyr --mr main <workspace_location>


As a module in an existing workspace
------------------------------------

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

   $ west update

Note: The meshtastic-zephyr module requires Zephyr's ``nanopb`` module to be present in the west
workspace (which should be the case by default).

Sample
******

Build the Meshtastic sample with:

.. code-block:: console

   $ west build -b <board> samples/meshtastic

Refer to the [sample's README](samples/meshtastic/README.rst) for more information on how to build
and run the sample and enable additional features.

License
*******

This project is licensed under GPL-3.0, similar to the original Meshtastic firmware.
