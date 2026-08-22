cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(meshtastic_airtime_tests)

include(${CMAKE_CURRENT_SOURCE_DIR}/../common/meshtastic_test.cmake)
meshtastic_test_setup()

target_sources(app PRIVATE src/main.c)
