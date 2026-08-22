cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(meshtastic_module_tests)

include(${CMAKE_CURRENT_SOURCE_DIR}/../common/meshtastic_test.cmake)
meshtastic_test_setup()
meshtastic_test_use_mock_lora()

target_sources(app PRIVATE
  src/modules_fixture.c
  src/fake_sensor.c
  src/telemetry.c
  src/nodeinfo.c
  src/message.c
)
