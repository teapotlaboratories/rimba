# rimba-halow-scan — first MM6108 bring-up: scan for HaLow APs.
# Based on the morsemicro/halow component's own scan example, with Seeed XIAO
# ESP32-S3 board overrides applied via sdkconfig.defaults.
cmake_minimum_required(VERSION 3.16)

# The vendored Morse Micro halow component lives at the repo root in components/ —
# shared across firmware apps — so add the repo-root components/ dir explicitly.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

# Morse firmware: a component generated under the build dir from vendor/morse-firmware
# (no firmware blobs are vendored). See cmake/mm-fw-gen/morse_firmware.cmake.
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/mm-fw-gen/morse_firmware.cmake")
morse_firmware_generate_component()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rimba_halow_scan)
