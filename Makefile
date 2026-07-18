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

# Optional per-app build arguments threaded to idf.py as CMake cache vars. Two consumers today, both
# fed by the regtest harness from the manifest registries (so no MAC/IP is hardcoded in firmware):
#   - test-mesh-linux interop peer:
#       make flash APP=test-mesh-linux ... LINUX_MAC=3c:22:7f:37:51:38 LINUX_IP=10.9.9.2
#   - test-mesh-relay line topology (all three, to the same symmetric app):
#       make flash APP=test-mesh-relay ... \
#         MESH_ORIGIN_MAC=e2:72:a1:f8:ef:a4 MESH_DEST_MAC=e2:72:a1:f8:f9:40 MESH_RELAY_MAC=e2:72:a1:f8:f0:08
# Empty = the app's built-in #ifndef defaults apply.
IDF_EXTRA_D := $(if $(LINUX_MAC),-D TEST_LINUX_MAC="$(LINUX_MAC)") \
               $(if $(LINUX_IP),-D TEST_LINUX_IP="$(LINUX_IP)") \
               $(if $(MESH_ORIGIN_MAC),-D TEST_MESH_ORIGIN_MAC="$(MESH_ORIGIN_MAC)") \
               $(if $(MESH_DEST_MAC),-D TEST_MESH_DEST_MAC="$(MESH_DEST_MAC)") \
               $(if $(MESH_RELAY_MAC),-D TEST_MESH_RELAY_MAC="$(MESH_RELAY_MAC)") \
               $(if $(HOST_LIGHT_SLEEP),-D TEST_HOST_LIGHT_SLEEP=$(HOST_LIGHT_SLEEP))

# Source the IDF environment, enter the app dir, then run idf.py with our
# out-of-source build directory and the selected board + app config defaults
# (-B / -D apply to build/flash/monitor/size/clean).
IDF := source "$(IDF_PATH)/export.sh" >/dev/null 2>&1 && cd "$(APP_DIR)" && \
       idf.py -B "$(BUILD_DIR)" \
              -D SDKCONFIG="$(BUILD_DIR)/sdkconfig" \
              -D SDKCONFIG_DEFAULTS="$(SDKCONFIG_DEFAULTS)" \
              $(IDF_EXTRA_D)

# Regression suite (tools/regtest/). BOARD_NAME selects the physical bench node for
# the hardware tiers (board2 is the only fully-wired one — see
# docs/reference/rimba-bench-devices.md), as opposed to BOARD, which selects the
# sdkconfig overlay.
TEST    := python3 $(CURDIR)/tools/regtest/run.py
BOARD_NAME ?= board2

.PHONY: help build flash monitor flash-monitor clean fullclean \
        menuconfig size erase \
        test test-t0 test-t1 test-t2 test-tp test-bench test-conn test-silence test-report

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

# ---- Regression suite ------------------------------------------------------
# Tiers, and what each proves:
#   test-t0  build matrix — every app x board compiles + carries a real country code.
#            No hardware. Catches most port-forward breakage. Proves nothing at runtime.
#   test-t1  smoke — flash + boot + the radio really comes up (chip id / fw / MAC /
#            runtime country). Needs a board. Proves nothing on the air.
#   test-t2  on-air feature tests — the milestone claims that actually matter.
#            Needs a rig (multiple boards and/or Linux nodes).
# See tools/regtest/README.md.

test: test-t0 test-t1          ## build matrix + smoke

test-t0:                       ## build matrix (no hardware)
	$(TEST) t0

test-t1:                       ## smoke on BOARD_NAME (default board2)
	$(TEST) t1 --board-name $(BOARD_NAME)

test-t2:                       ## on-air feature tests (needs a rig)
	$(TEST) t2

test-tp:                       ## PPK2 power-save tier (needs board2 + PPK2 + C6 + an AP)
	$(TEST) tp $(if $(AP),--ap $(AP),)

test-bench:                    ## what hardware is present right now
	$(TEST) bench

# Preflight: check you can actually reach a device before a (long) test run. Pass YOUR
# setup info; with no args it probes every bench node the manifest knows.
#   make test-conn PORT=/dev/ttyACM0     # a board on a serial port
#   make test-conn MAC=E0:72:A1:F8:F0:08 # a board by efuse MAC (resolves the port)
#   make test-conn NODE=board2           # a manifest bench node
#   make test-conn HOST=chronite         # a Linux node over ssh
# PORT has a default (for flash), so it is only forwarded when set ON the command line.
test-conn:                     ## check a device is reachable (PORT=|MAC=|NODE=|HOST=)
	$(TEST) preflight \
	  $(if $(filter command line,$(origin PORT)),--port $(PORT),) \
	  $(if $(MAC),--mac $(MAC),) \
	  $(if $(NODE),--node $(NODE),) \
	  $(if $(HOST),--host $(HOST),) \
	  $(if $(CHIP),--chip $(CHIP),)

test-silence:                  ## return every ESP to the radio-free idle app
	$(TEST) silence

test-report:                   ## (re)generate build/regtest/report.html from the latest baselines
	$(TEST) report
