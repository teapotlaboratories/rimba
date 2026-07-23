# test-mesh-ae — 802.11s 6-address Address-Extension (AE_A5_A6) datapath probe (S3).
# Injects proxied AE mesh data frames to a Linux mesh node; verified via `iw mpp dump`. From test-mesh-gate-rx.
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/mm-fw-gen/morse_firmware.cmake")
morse_firmware_generate_component()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(test_mesh_ae)
