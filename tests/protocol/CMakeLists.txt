cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(meshtastic_protocol_tests)

target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src
  ${CMAKE_BINARY_DIR}/modules/meshtastic-zephyr/src
  ${CMAKE_BINARY_DIR}/modules/meshtastic-zephyr/src/proto
)

target_sources(app PRIVATE src/main.c)
