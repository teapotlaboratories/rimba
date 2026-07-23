# test-mesh-gate — 802.11s mesh-gate RANN emitter (S1 verification fixture).
# Brings up a secured mesh node and floods PROACTIVE_RANN + gate announcements, for the on-air
# byte-diff against a live Linux gate. Derived from rimba-halow-mesh.
cmake_minimum_required(VERSION 3.16)

# Vendored Morse Micro components live at the repo root (shared across apps).
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

# Morse firmware: a component generated under the build dir from vendor/morse-firmware.
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/mm-fw-gen/morse_firmware.cmake")
morse_firmware_generate_component()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(test_mesh_gate)
