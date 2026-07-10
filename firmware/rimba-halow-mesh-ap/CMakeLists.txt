# rimba-halow-mesh-ap — Mesh + SoftAP concurrent on ONE MM6108 (the all-ESP32
# "Mesh-gate", §A3 Stage 3 bring-up). Mesh on the primary vif, SoftAP on a
# secondary vif, co-channel. Needs the components/halow feat/mesh-ap-concurrency
# branch (Stage 1 + Stage 2 per-vif beacon). See
# docs/mesh-ap/rimba-mesh-ap-esp32-stage1-codemap.md.
cmake_minimum_required(VERSION 3.16)

# Vendored Morse Micro components live at the repo root (shared across apps).
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

# Morse firmware: a component generated under the build dir from vendor/morse-firmware
# (no firmware blobs are vendored). See cmake/mm-fw-gen/morse_firmware.cmake.
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/mm-fw-gen/morse_firmware.cmake")
morse_firmware_generate_component()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rimba_halow_mesh_ap)
