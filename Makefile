# Rimba firmware — convenience wrapper around ESP-IDF's idf.py.
#
# ESP-IDF is vendored at vendor/esp-idf (submodule, pinned v5.4.2). This Makefile
# sources its environment for each target so you don't have to `. export.sh`
# by hand. Override any variable on the command line, e.g.:
#
#   make build                       # default app rimba-halow-scan, board proto1-fgh100m
#   make build APP=rimba-hello
#   make build BOARD=proto1-fgh100m
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
# override with BOARD=<name> (default: proto1-fgh100m, the bench board). The board's
# sdkconfig.defaults (plus sdkconfig.defaults.<target>) are applied to the build via
# SDKCONFIG_DEFAULTS. The chip target is NOT set here — it comes from the board's
# sdkconfig.defaults (CONFIG_IDF_TARGET), so the board owns its target.
BOARD     ?= proto1-fgh100m
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

# Regression suite (tools/regtest/). ALWAYS run the suite through these `make test-*`
# targets — make sources the vendored ESP-IDF env for you (pyserial / ppk2-api), so no
# `source export.sh` is ever needed.
#
# The BENCH IDENTITY is passed in — NOTHING about the bench is stored in the Python source.
# Provide these per run, or `export` them once in your shell (make forwards them either way);
# a hardware tier that's missing one errors and tells you what to pass:
#   BENCH_BOARD  sdkconfig overlay for the ESP boards (e.g. proto1-fgh100m)
#   BOARD0_MAC   board0 efuse MAC   BOARD1_MAC board1 efuse MAC   BOARD2_MAC board2 efuse MAC
#   WIRED_BOARD  which board is fully wired (WAKE+BUSY), normally board2
#   LINUX_HOST / LINUX_MAC / LINUX_IP   the Linux interop node (only the T2 interop tests + tp --ap linux need it)
# Test-run choices (also no defaults): BOARD_NAME (test-t1), AP (test-tp), CYCLES (test-dscycle).
BENCH_ENV := BENCH_BOARD='$(BENCH_BOARD)' BOARD0_MAC='$(BOARD0_MAC)' BOARD1_MAC='$(BOARD1_MAC)' \
             BOARD2_MAC='$(BOARD2_MAC)' WIRED_BOARD='$(WIRED_BOARD)' \
             LINUX_HOST='$(LINUX_HOST)' LINUX_MAC='$(LINUX_MAC)' LINUX_IP='$(LINUX_IP)'
RUNNER  := source "$(IDF_PATH)/export.sh" >/dev/null 2>&1 && $(BENCH_ENV) python3 $(CURDIR)/tools/regtest/run.py

# require-var,<VAR>,<hint> — abort a target if a required test parameter is unset.
require-var = @[ -n "$($(1))" ] || { echo "$@ requires $(1)=$(2)"; exit 2; }

.PHONY: help build flash monitor flash-monitor clean fullclean \
        menuconfig size erase \
        test test-all test-t0 test-t1 test-t2 test-tp test-dscycle test-bench \
        test-conn test-interop test-silence test-report test-unit test-lint test-flakes test-trend

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

test:                          ## build matrix + smoke (BOARD_NAME=board0|board1|board2 required)
	$(call require-var,BOARD_NAME,board0|board1|board2)
	@$(MAKE) test-t0
	@$(MAKE) test-t1 BOARD_NAME=$(BOARD_NAME)

# The whole bench, every tier in order. Runs them ALL even if one tier FAILs, then regenerates the
# report; exits non-zero if any tier had a real FAIL (INCONCLUSIVE / SKIP do not gate). Needs the full
# rig: board2 powered (tools/ppk2_hold.py) + the C6 flashed (firmware/test-c6-trigger, serial-controlled),
# plus board0/board1 for the T2 multi-board tests. tp runs before dscycle and frees the PPK2 holder, but
# dscycle now self-starts ppk2_hold if board2 isn't powered, so it no longer SKIPs silently after tp.
test-all:                      ## run EVERY tier t0->t1->t2->tp->dscycle (BOARD_NAME, AP, CYCLES required)
	$(call require-var,BOARD_NAME,board0|board1|board2)
	$(call require-var,AP,esp|linux)
	$(call require-var,CYCLES,a positive integer e.g. 2)
	@$(MAKE) test-lint || { echo "test-all: lint gate FAILED — fix the harness before a bench run"; exit 2; }; \
	rc=0; \
	$(MAKE) test-t0                            || rc=1; \
	$(MAKE) test-t1 BOARD_NAME=$(BOARD_NAME)   || rc=1; \
	$(MAKE) test-t2                            || rc=1; \
	$(MAKE) test-tp AP=$(AP)                   || rc=1; \
	$(MAKE) test-dscycle CYCLES=$(CYCLES)      || rc=1; \
	$(MAKE) test-report || true; \
	[ $$rc -eq 0 ] && echo "test-all: all tiers green" || echo "test-all: a tier FAILed (see above)"; \
	exit $$rc

test-t0:                       ## build matrix (no hardware)
	$(RUNNER) t0

test-unit:                     ## harness unit tests (no hardware/bench; IDF env for pyflakes/pyserial)
	@source "$(IDF_PATH)/export.sh" >/dev/null 2>&1; \
	python3 -m unittest discover -s $(CURDIR)/tools/regtest/tests -p 'test_*.py'

test-lint:                     ## pyflakes lint gate over the harness (no hardware; runs before every tier too)
	$(RUNNER) lint

test-flakes:                   ## run-history flake ledger — which tests flip verdict run-to-run (no hardware)
	$(RUNNER) flakes

test-trend:                    ## trend recorded numeric metrics over runs (TEST=slug LAST=N), or DIFF="A B" across gitlinks
	$(RUNNER) trend $(if $(TEST),--test $(TEST),) $(if $(LAST),--last $(LAST),) $(if $(DIFF),--diff $(DIFF),)

test-t1:                       ## smoke on BOARD_NAME=board0|board1|board2 (required); INCLUDE_SLEEP=1 adds the sleep apps (board2)
	$(call require-var,BOARD_NAME,board0|board1|board2)
	$(RUNNER) t1 --board-name $(BOARD_NAME) $(if $(INCLUDE_SLEEP),--include-sleep-apps,)

test-t2:                       ## on-air tests; TEST="slug ..." runs those, DRY_RUN=1 lists, APPEND=1 accumulates
	$(RUNNER) t2 $(foreach t,$(TEST),--test $(t)) $(if $(DRY_RUN),--dry-run,) $(if $(APPEND),--append,) \
	  $(if $(LINUX_MAC),--linux-mac $(LINUX_MAC),) $(if $(LINUX_IP),--linux-ip $(LINUX_IP),)

test-tp:                       ## PPK2 power tier — AP=esp|linux (required); LIGHT_SLEEP=1, DRY_RUN=1
	$(call require-var,AP,esp|linux)
	$(RUNNER) tp --ap $(AP) $(if $(LIGHT_SLEEP),--light-sleep,) $(if $(DRY_RUN),--dry-run,)

test-dscycle:                  ## deep-sleep reconnect gate — CYCLES=N required (e.g. CYCLES=2)
	$(call require-var,CYCLES,a positive integer e.g. 2)
	$(RUNNER) dscycle --cycles $(CYCLES)

test-bench:                    ## what hardware is present right now
	$(RUNNER) bench

# Preflight: check you can actually reach a device before a (long) test run. Pass YOUR
# setup info; with no args it probes every bench node the manifest knows.
#   make test-conn PORT=/dev/ttyACM0     # a board on a serial port
#   make test-conn MAC=E0:72:A1:F8:F0:08 # a board by efuse MAC (resolves the port)
#   make test-conn NODE=board2           # a manifest bench node
#   make test-conn HOST=chronite         # a Linux node over ssh
# PORT has a default (for flash), so it is only forwarded when set ON the command line.
test-conn:                     ## check a device is reachable (PORT=|MAC=|NODE=|HOST=)
	$(RUNNER) preflight \
	  $(if $(filter command line,$(origin PORT)),--port $(PORT),) \
	  $(if $(MAC),--mac $(MAC),) \
	  $(if $(NODE),--node $(NODE),) \
	  $(if $(HOST),--host $(HOST),) \
	  $(if $(CHIP),--chip $(CHIP),)

# ESP<->Linux mesh interop: flash test-mesh-linux at a Linux node, its MAC auto-queried over ssh.
#   make test-interop HOST=chronite PORT=/dev/ttyACM4         # flash against chronite
#   make test-interop HOST=chronite PORT=/dev/ttyACM4 GO=1    # + bring the mesh up, capture the verdict, silence
test-interop:                  ## flash test-mesh-linux at HOST= over PORT= (GO=1 to run it + get the verdict)
	$(call require-var,HOST,an ssh host e.g. chronite)
	@[ -n "$(filter command line,$(origin PORT))" ] || { echo "$@ requires PORT=/dev/ttyACMx (set it explicitly)"; exit 2; }
	$(RUNNER) flash-interop --host $(HOST) --port $(PORT) $(if $(GO),--run,) $(if $(MONITOR),--monitor,) \
	  $(if $(IP),--ip $(IP),) $(if $(LINUX_MAC),--linux-mac $(LINUX_MAC),)

test-silence:                  ## return every ESP to the radio-free idle app
	$(RUNNER) silence

test-report:                   ## (re)generate build/regtest/report.html from the latest baselines
	$(RUNNER) report
