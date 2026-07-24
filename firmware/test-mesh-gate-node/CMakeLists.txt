# test-mesh-gate-node — dual-mode mesh node (retire-L3 + proxy-ARP): DEFAULT = the mesh-gate-b2 (Case D)
# pinger reporter; NO_PING=1 = a silent static mesh responder (the mesh-gate-bridge / Case C support role).
# Derived from test-mesh-relay + test-mesh-gate-fwd.
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/mm-fw-gen/morse_firmware.cmake")
morse_firmware_generate_component()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(test_mesh_gate_node)
