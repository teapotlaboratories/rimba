# test-idle — the radio-silent resting-state fixture (manifest.IDLE_APP).
# The regression harness flashes this after every hardware test so the board is
# left radio-free. Cloned from rimba-hello; standard ESP-IDF project skeleton.
cmake_minimum_required(VERSION 3.16)

# Vendored Morse Micro components live at the repo root (shared across apps).
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

# Morse firmware component (generated under the build dir from vendor/morse-firmware).
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/mm-fw-gen/morse_firmware.cmake")
morse_firmware_generate_component()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(test_idle)
