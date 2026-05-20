# SPDX-License-Identifier: GPL-3.0

zephyr_include_directories(include)

# Some Zephyr trees may still carry this subsystem in-tree during transition.
if(NOT TARGET meshtastic)
  add_subdirectory(src)
endif()
