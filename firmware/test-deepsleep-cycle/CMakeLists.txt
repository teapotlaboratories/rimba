# test-deepsleep-cycle — the dscycle-tier DUT fixture: a deep-sleep leaf that wakes on the
# C6 trigger (test-c6-trigger) and re-associates to the AP, emitting the "RECONNECTED in <ms>"
# line tools/regtest/dscycle.py scrapes. Verbatim clone of the rimba-deepsleep-cycle example, with
# all bench-rig hooks retained (C6/D5 EXT1 wake, flash-hold guard, flash window). Built on the
# vendored morsemicro/halow component.
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

# Morse firmware: a component generated under the build dir from vendor/morse-firmware.
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/mm-fw-gen/morse_firmware.cmake")
morse_firmware_generate_component()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(test_deepsleep_cycle)
