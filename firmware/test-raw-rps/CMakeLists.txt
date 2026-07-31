# test-raw-rps — does a host-spliced RPS element reach the air? (RAW S0b-3/S0b-4). Single board + sniffer.
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/mm-fw-gen/morse_firmware.cmake")
morse_firmware_generate_component()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# The S0b spike gate. This is deliberately a BUILD-GLOBAL compile definition, not an app-private one:
# the two guarded edits live in the components/halow submodule (morselib umac_ap.c and
# s1g_capabilities.c), not under main/, so the app-private `target_compile_definitions(${COMPONENT_LIB}
# PRIVATE ...)` idiom the other fixtures use would never reach them. `idf_build_set_property` is read by
# `component.cmake` as a generator expression and applied with directory scope inside every
# `idf_component_register`, which is how ESP_PLATFORM/IDF_VER already land on morselib's compile lines.
#
# Scoped to THIS app: every app gets its own build tree (Makefile:39 build/<APP>/<BOARD>), so the other
# 26 apps that set CONFIG_HALOW_AP_MODE=y never see the define and never emit an RPS.
#
# Why not a CMake cache var via the Makefile's IDF_EXTRA_D: cache entries are STICKY per build dir, and
# tools/regtest/t0_build.py rebuilds each app into that same dir with no vars — so one spike build would
# silently keep the RPS compiled into T0 until a fullclean.
idf_build_set_property(COMPILE_DEFINITIONS "RIMBA_RAW_S0B_SPIKE=1" APPEND)

project(test_raw_rps)
