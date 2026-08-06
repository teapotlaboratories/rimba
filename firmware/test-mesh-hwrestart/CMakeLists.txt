# test-mesh-hwrestart — S1 of the mesh hw_restart work: force a chip restart on demand and report
# whether the mesh vif and its peers come back. See docs/mesh-ap/rimba-mesh-hw-restart-design.md.
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/mm-fw-gen/morse_firmware.cmake")
morse_firmware_generate_component()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(test_mesh_hwrestart)
