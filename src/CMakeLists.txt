# SPDX-License-Identifier: GPL-3.0

if(CONFIG_MESHTASTIC)

  zephyr_library_named(meshtastic)

  list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
  include(nanopb)
  set(NANOPB_IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/proto)

  # Generate C sources from the Meshtastic protobuf schemas.
  set(MESHTASTIC_NANOPB_PROTOS
    proto/meshtastic/atak.proto
    proto/meshtastic/channel.proto
    proto/meshtastic/config.proto
    proto/meshtastic/deviceonly.proto
    proto/meshtastic/device_ui.proto
    proto/meshtastic/module_config.proto
    proto/meshtastic/portnums.proto
    proto/meshtastic/telemetry.proto
    proto/meshtastic/xmodem.proto
    proto/meshtastic/mesh.proto
  )
  if(CONFIG_MESHTASTIC_MQTT)
    list(APPEND MESHTASTIC_NANOPB_PROTOS proto/meshtastic/mqtt.proto)
  endif()
  zephyr_nanopb_sources(meshtastic ${MESHTASTIC_NANOPB_PROTOS})

  target_include_directories(meshtastic PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/proto
    ${CMAKE_CURRENT_SOURCE_DIR}
  )

  zephyr_linker_sources(SECTIONS iterables.ld)

  zephyr_library_sources(
    meshtastic.c
    meshtastic_channels.c
    meshtastic_config_store.c
    meshtastic_modules.c
    meshtastic_packet.c
    meshtastic_radio.c
    meshtastic_outbound.c
    meshtastic_router.c
    meshtastic_routing.c
    meshtastic_settings_apply.c
  )
  zephyr_library_sources_ifdef(CONFIG_MESHTASTIC_SETTINGS meshtastic_settings.c)
  zephyr_library_sources_ifdef(CONFIG_MESHTASTIC_PHONEAPI
    meshtastic_phoneapi.c
    meshtastic_phoneapi_config.c
  )
  zephyr_library_sources_ifdef(CONFIG_MESHTASTIC_BLE meshtastic_ble.c)
  zephyr_library_sources_ifdef(CONFIG_MESHTASTIC_SERIAL meshtastic_serial.c)
  zephyr_library_sources_ifdef(CONFIG_MESHTASTIC_SHELL meshtastic_shell.c)
  zephyr_library_sources_ifdef(CONFIG_MESHTASTIC_NODEDB meshtastic_nodedb.c)
  zephyr_library_sources_ifdef(CONFIG_MESHTASTIC_GNSS meshtastic_gnss.c)
  zephyr_library_sources_ifdef(CONFIG_MESHTASTIC_AIRTIME meshtastic_airtime.c)
  zephyr_library_sources_ifdef(CONFIG_MESHTASTIC_DEVICE_METRICS meshtastic_metrics.c)
  zephyr_library_sources_ifdef(CONFIG_MESHTASTIC_ENVIRONMENT_METRICS meshtastic_environment.c)
  zephyr_library_sources_ifdef(CONFIG_MESHTASTIC_TELEMETRY_WANT_RESPONSE meshtastic_telemetry_common.c)
  zephyr_library_sources_ifdef(CONFIG_MESHTASTIC_NODEINFO meshtastic_nodeinfo.c)
  zephyr_library_sources_ifdef(CONFIG_MESHTASTIC_MESSAGE meshtastic_message.c)
  zephyr_library_sources_ifdef(CONFIG_MESHTASTIC_MQTT meshtastic_mqtt.c)

endif()
