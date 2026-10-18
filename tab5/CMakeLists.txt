# Catalyst Tab — ESP-IDF project for the M5Stack Tab5 (ESP32-P4 + ESP32-C6).
#
#   . $IDF_PATH/export.sh && idf.py set-target esp32p4 && idf.py build flash monitor
#
# The UI, renderer, NT4 client and Catalyst model are portable C shared with the host simulator (sim/);
# only components/hal/src/hal_tab5.c and main/ touch ESP-IDF.
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/components)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# One LVGL configuration for the firmware and the simulator
idf_build_set_property(COMPILE_OPTIONS "-DLV_CONF_PATH=\"${CMAKE_CURRENT_LIST_DIR}/lv_conf.h\"" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-truncation" APPEND)
project(catalyst_tab)
