# test-mesh-gate-rx — 802.11s mesh-gate RANN RX + relay (S2 verification fixture).
# Peers with a Linux mesh gate, learns it via its RANN, re-floods, and advertises Connected-to-Gate.
# Derived from test-mesh-linux + test-mesh-gate.
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/mm-fw-gen/morse_firmware.cmake")
morse_firmware_generate_component()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(test_mesh_gate_rx)
