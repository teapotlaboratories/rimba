# rimba-halow-ibss — RISK-01 IBSS / ad-hoc command-acceptance test.
# Built on the vendored morsemicro/halow component (with the local IBSS additions
# to morselib). See docs/worklog/2026-06-18-risk01-ibss-recon.md.
cmake_minimum_required(VERSION 3.16)

# Vendored Morse Micro components live at the repo root (shared across apps).
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rimba_halow_ibss)
