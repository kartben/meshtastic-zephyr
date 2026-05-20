.. _meshtastic-sample:

Meshtastic
##########

Overview
********

This sample demonstrates the Zephyr-native Meshtastic stack.  It initializes the stack and then
registers a receive callback that logs incoming text messages and broadcasts a greeting at regular
interval on the default "LongFast" channel.

Requirements
************

* A board with a LoRa transceiver supported by the Zephyr, available as ``lora0`` devicetree alias.

* The ``nanopb`` module must be present in the west workspace

Building and Running
********************

By default the node ID is derived from the hardware device ID (using HWINFO driver). To use a fixed
ID instead, enable custom source and set the default:

.. code-block:: console

   west build -b <your_board> samples/meshtastic -- \
     -DCONFIG_MESHTASTIC_NODE_ID_CUSTOM=y \
     -DCONFIG_MESHTASTIC_NODE_ID_DEFAULT=0x01020304

The sample can also be built for the LoRa radio emulator on ``native_sim``
by adding the appropriate overlay.

Shell commands (TBC)
********************

When ``CONFIG_MESHTASTIC_SHELL=y``), the following shell commands are available:

* ``meshtastic status`` — node counters, primary channel hash, device role, rebroadcast mode
* ``meshtastic channel list`` / ``channel show <0-7>`` — channel table
* ``meshtastic channel set <index> name <str> role secondary psk default`` — runtime channel edit (RAM only)
* ``meshtastic channel disable <index>`` — disable a slot
* ``meshtastic device role [client|router|...]`` — mesh device role
* ``meshtastic device rebroadcast [all|none|local_only|...]`` — relay policy
* ``meshtastic text send [-c <index>] [dest|broadcast] <message>`` — send on a specific channel

More commands are available when additional features (e.g. environment metrics, GNSS, ...).

Sample Output
*************

.. code-block:: console

   [00:00:00.000] <inf> meshtastic: Meshtastic init: node=0xdeadbeef ch_hash=0x08 freq=906875000Hz
   [00:00:00.001] <inf> meshtastic_sample: Meshtastic sample started, node ID 0xdeadbeef
   [00:00:02.345] <inf> meshtastic_sample: MSG from 0xc0ffee42: Hello mesh!  (RSSI -87 dBm, SNR 7)
   [00:00:30.001] <inf> meshtastic: TX to 0xffffffff port=1 len=29
   [00:00:30.001] <inf> meshtastic_sample: Broadcast sent
