# rimba-halow-mesh — 802.11s mesh P0 firmware-feasibility probe.
# Asks the MM6108 blob whether it recognizes interface_type MESH(5) (and the AP/
# ADHOC/bogus controls), the gate before the multi-PR mesh port (P1-P5).
# See docs/worklog/2026-06-24-mesh-80211s-port-recon.md.
cmake_minimum_required(VERSION 3.16)

# Vendored Morse Micro components live at the repo root (shared across apps).
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rimba_halow_mesh)
