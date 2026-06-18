# Rimba firmware — convenience wrapper around ESP-IDF's idf.py.
#
# ESP-IDF is installed out-of-tree (default: $HOME/esp/esp-idf). This Makefile
# sources its environment for each target so you don't have to `. export.sh`
# by hand. Override any variable on the command line, e.g.:
#
#   make build APP=rimba-hello
#   make flash PORT=/dev/ttyACM0
#   make flash-monitor
#
SHELL := /bin/bash

# ESP-IDF 5.4.2: required by the morsemicro/halow component (>=5.4.2).
IDF_PATH  ?= $(HOME)/esp/esp-idf-5.4.2
PORT      ?= /dev/ttyACM0
TARGET    ?= esp32s3
APP       ?= rimba-hello
APP_DIR   := $(CURDIR)/firmware/$(APP)

# All build output goes to <repo root>/build/<APP>/, never inside the app dir.
BUILD_DIR := $(CURDIR)/build/$(APP)

# Source the IDF environment, enter the app dir, then run idf.py with our
# out-of-source build directory (-B applies to build/flash/monitor/size/clean).
IDF := source "$(IDF_PATH)/export.sh" >/dev/null 2>&1 && cd "$(APP_DIR)" && \
       idf.py -B "$(BUILD_DIR)"

.PHONY: help set-target build flash monitor flash-monitor clean fullclean \
        menuconfig size erase

help:
	@echo "Rimba firmware build (ESP-IDF wrapper)"
	@echo "  make set-target    - set chip target ($(TARGET))"
	@echo "  make build         - build $(APP)"
	@echo "  make flash         - flash $(APP) to $(PORT)"
	@echo "  make monitor       - serial monitor on $(PORT)  (Ctrl-] to exit)"
	@echo "  make flash-monitor - flash then monitor"
	@echo "  make size          - binary size breakdown"
	@echo "  make clean | fullclean | erase | menuconfig"
	@echo ""
	@echo "Apps live under firmware/<APP>/; build output in build/<APP>/.  Vars:"
	@echo "  APP=$(APP)  TARGET=$(TARGET)  PORT=$(PORT)"
	@echo "  IDF_PATH=$(IDF_PATH)"
	@echo "  BUILD_DIR=$(BUILD_DIR)"

set-target:
	$(IDF) set-target $(TARGET)

build:
	$(IDF) build

flash:
	$(IDF) -p $(PORT) flash

monitor:
	$(IDF) -p $(PORT) monitor

flash-monitor:
	$(IDF) -p $(PORT) flash monitor

size:
	$(IDF) size

clean:
	$(IDF) clean

fullclean:
	$(IDF) fullclean

erase:
	$(IDF) -p $(PORT) erase-flash

menuconfig:
	$(IDF) menuconfig
