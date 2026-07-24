# test-mesh-gate-node — T2 mesh-gate-b2 mesh-node reporter (retire-L3 + proxy-ARP).
# (NODE<->RELAY<->GATE forced line). Derived from test-mesh-relay + test-mesh-gate-fwd.
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/mm-fw-gen/morse_firmware.cmake")
morse_firmware_generate_component()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(test_mesh_gate_node)
