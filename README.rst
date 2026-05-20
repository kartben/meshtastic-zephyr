Meshtastic Zephyr Module
########################

This repository packages Zephyr's Meshtastic mesh radio stack as an external
Zephyr module. It contains the Meshtastic implementation, public headers,
protobuf schemas used for nanopb generation, and the Meshtastic sample.

The module is intended to be used from a Zephyr west workspace.

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
