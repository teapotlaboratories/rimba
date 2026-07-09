# rimba-deepsleep-cycle — deep-sleep leaf that wakes on the C6 trigger and re-associates to the AP.
# Built on the vendored morsemicro/halow component (same skeleton as rimba-halow-sta).
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

# Morse firmware: a component generated under the build dir from vendor/morse-firmware.
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/mm-fw-gen/morse_firmware.cmake")
morse_firmware_generate_component()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rimba_deepsleep_cycle)
