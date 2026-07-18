# test-power — the power-save ladder DUT for the `tp` (power) regression tier.
# A C6-triggered 4-tier PS ladder (No-PS / Dyn-PS / TWT / WNM+powerdown) whose per-phase
# current is measured host-side off the PPK2 by tools/regtest/tp_power.py. Derived from
# rimba-halow-sta; reports on the TEST| console (no RESULT — the verdict is the mA).
cmake_minimum_required(VERSION 3.16)

# Vendored Morse Micro components live at the repo root (shared across apps).
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

# Morse firmware: a component generated under the build dir from vendor/morse-firmware.
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/mm-fw-gen/morse_firmware.cmake")
morse_firmware_generate_component()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(test_power)
