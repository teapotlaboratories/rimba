# test-mesh-gate-ap — the GATE fixture (retire-L3 mesh+AP L2 bridge) for the mesh-gate T2 tests.
# HaLow mesh on the primary vif and a co-channel SoftAP on a secondary vif, on ONE
# MM6108, bridged as a pure L2 gate on one flat 10.9.9.0/24 subnet (DHCP + proxy-ARP,
# no ip_forward). A test-* clone of the rimba-halow-mesh-ap example. See main/app_main.c.
cmake_minimum_required(VERSION 3.16)

# Vendored Morse Micro components live at the repo root (shared across apps).
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

# Morse firmware: a component generated under the build dir from vendor/morse-firmware
# (no firmware blobs are vendored). See cmake/mm-fw-gen/morse_firmware.cmake.
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/mm-fw-gen/morse_firmware.cmake")
morse_firmware_generate_component()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(test_mesh_gate_ap)
