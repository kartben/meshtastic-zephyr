cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(meshtastic_shell_tests)

include(${CMAKE_CURRENT_SOURCE_DIR}/../common/meshtastic_test.cmake)
meshtastic_test_setup()
meshtastic_test_use_mock_lora()

target_sources(app PRIVATE
  src/channel_psk.c
  src/commands.c
  src/names.c
  src/nodedb_show.c
  src/shell_fixture.c
)
