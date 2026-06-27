# rimba-halow-meshscan — verification sniffer for the 802.11s mesh node.
# Listens for raw beacons over the air and flags the one carrying our Mesh ID IE,
# hexdumping it so the Mesh ID + Mesh Configuration IEs can be decoded. Pair with
# firmware/rimba-halow-mesh running on another board.
cmake_minimum_required(VERSION 3.16)

# The vendored Morse Micro halow component lives at the repo root in components/.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

# Morse firmware: a component generated under the build dir from vendor/morse-firmware.
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/mm-fw-gen/morse_firmware.cmake")
morse_firmware_generate_component()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rimba_halow_meshscan)
