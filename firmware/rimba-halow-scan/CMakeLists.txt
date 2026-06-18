# rimba-halow-scan — first MM6108 bring-up: scan for HaLow APs.
# Based on the morsemicro/halow component's own scan example, with Seeed XIAO
# ESP32-S3 board overrides applied via sdkconfig.defaults.
cmake_minimum_required(VERSION 3.16)

# The vendored Morse Micro components (halow, firmware) live at the repo root in
# components/ — shared across firmware apps — so they are not auto-discovered from
# this project's dir. Add the repo-root components/ dir explicitly.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rimba_halow_scan)
