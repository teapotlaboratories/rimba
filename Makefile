# Rimba firmware — convenience wrapper around ESP-IDF's idf.py.
#
# ESP-IDF is vendored at vendor/esp-idf (submodule, pinned v5.4.2). This Makefile
# sources its environment for each target so you don't have to `. export.sh`
# by hand. Override any variable on the command line, e.g.:
#
#   make build                       # default app rimba-halow-scan, board proto1
#   make build APP=rimba-hello
#   make build BOARD=proto1
#   make flash PORT=/dev/ttyACM0
#   make flash-monitor
#
SHELL := /bin/bash

# ESP-IDF 5.4.2 (>=5.4.2 required by the morsemicro/halow component) is vendored
# as a submodule at vendor/esp-idf. Override IDF_PATH to use a different install.
IDF_PATH  ?= $(CURDIR)/vendor/esp-idf
PORT      ?= /dev/ttyACM0
APP       ?= rimba-halow-scan
APP_DIR   := $(CURDIR)/firmware/$(APP)

# Board selection. Board-specific sdkconfig defaults live in boards/<BOARD>/;
# override with BOARD=<name> (default: proto1). The board's sdkconfig.defaults
# (plus sdkconfig.defaults.<target>) are applied to the build via
# SDKCONFIG_DEFAULTS. The chip target is NOT set here — it comes from the board's
# sdkconfig.defaults (CONFIG_IDF_TARGET), so the board owns its target.
BOARD     ?= proto1
BOARD_DIR := $(CURDIR)/boards/$(BOARD)

# sdkconfig defaults applied to the build: the board overlay first, then the
# app's own sdkconfig.defaults (if it has one) layered on top so it can add
# app-level config (e.g. CONFIG_HALOW_AP_MODE). IDF auto-appends the matching
# .<target> variant of each file.
SDKCONFIG_DEFAULTS := $(BOARD_DIR)/sdkconfig.defaults$(if $(wildcard $(APP_DIR)/sdkconfig.defaults),;$(APP_DIR)/sdkconfig.defaults)

# All build output goes to <repo root>/build/<APP>/<BOARD>/, never inside the
# app dir. The generated sdkconfig lives there too, so each board keeps its own
# config and switching boards never reuses a stale one.
BUILD_DIR := $(CURDIR)/build/$(APP)/$(BOARD)

# Source the IDF environment, enter the app dir, then run idf.py with our
# out-of-source build directory and the selected board + app config defaults
# (-B / -D apply to build/flash/monitor/size/clean).
IDF := source "$(IDF_PATH)/export.sh" >/dev/null 2>&1 && cd "$(APP_DIR)" && \
       idf.py -B "$(BUILD_DIR)" \
              -D SDKCONFIG="$(BUILD_DIR)/sdkconfig" \
              -D SDKCONFIG_DEFAULTS="$(SDKCONFIG_DEFAULTS)"

.PHONY: help build flash monitor flash-monitor clean fullclean \
        menuconfig size erase

help:
	@echo "Rimba firmware build (ESP-IDF wrapper)"
	@echo "  make build         - build $(APP)"
	@echo "  make flash         - flash $(APP) to $(PORT)"
	@echo "  make monitor       - serial monitor on $(PORT)  (Ctrl-] to exit)"
	@echo "  make flash-monitor - flash then monitor"
	@echo "  make size          - binary size breakdown"
	@echo "  make clean | fullclean | erase | menuconfig"
	@echo ""
	@echo "Apps live under firmware/<APP>/; boards under boards/<BOARD>/;"
	@echo "build output in build/<APP>/<BOARD>/.  Vars:"
	@echo "  APP=$(APP)  BOARD=$(BOARD)  PORT=$(PORT)"
	@echo "  IDF_PATH=$(IDF_PATH)   (chip target comes from the board)"
	@echo "  BUILD_DIR=$(BUILD_DIR)"

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
