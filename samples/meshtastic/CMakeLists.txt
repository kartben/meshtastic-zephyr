# SPDX-License-Identifier: GPL-3.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(meshtastic_sample)

target_sources(app PRIVATE
  src/main.c
  src/sample_app.c
)

if(CONFIG_MESHTASTIC_SAMPLE_GUI)
  target_sources(app PRIVATE
    src/sample_gui.c
    src/sample_gui_model.c
    src/sample_gui_renderer.c
  )
else()
  target_sources(app PRIVATE src/sample_gui_stub.c)
endif()
