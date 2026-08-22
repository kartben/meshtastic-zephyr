cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(meshtastic_settings_tests)

include(${CMAKE_CURRENT_SOURCE_DIR}/../common/meshtastic_test.cmake)
meshtastic_test_setup()
meshtastic_test_use_mock_lora()

target_sources(app PRIVATE
  src/apply.c
  src/persistence.c
  src/settings_fixture.c
)
