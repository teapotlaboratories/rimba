# rimba-halow-mesh-monitor — 802.11s mesh-beacon monitor for MM6108 (see README.md).
cmake_minimum_required(VERSION 3.16)

# The vendored Morse Micro halow component lives at the repo root in components/.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

# Morse firmware: a component generated under the build dir from vendor/morse-firmware.
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/mm-fw-gen/morse_firmware.cmake")
morse_firmware_generate_component()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rimba_halow_mesh_monitor)
