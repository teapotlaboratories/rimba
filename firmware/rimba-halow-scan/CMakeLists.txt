# rimba-halow-scan — first MM6108 bring-up: scan for HaLow APs.
# Based on the morsemicro/halow component's own scan example, with Seeed XIAO
# ESP32-S3 board overrides applied via sdkconfig.defaults.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rimba_halow_scan)
