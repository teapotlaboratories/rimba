# rimba-halow-ap — HaLow SoftAP node for the 2-board ping test.
# Pair with rimba-halow-sta. Built on the vendored morsemicro/halow component.
cmake_minimum_required(VERSION 3.16)

# Vendored Morse Micro components live at the repo root (shared across apps).
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rimba_halow_ap)
