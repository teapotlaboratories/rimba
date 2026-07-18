# test-twt-assoc-sta — STA (requester+reporter) of the ASSOC-EMBEDDED TWT T2 test.
# Delta vs test-twt-sta: TWT is requested via mmwlan_twt_add_configuration BEFORE connect
# (the assoc-IE path a Linux hostapd honours), not a mid-session action frame.
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/mm-fw-gen/morse_firmware.cmake")
morse_firmware_generate_component()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(test_twt_assoc_sta)
