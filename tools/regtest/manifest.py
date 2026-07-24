"""Regression-suite manifest — the single source of truth for what gets tested.

Everything else in tools/regtest/ reads this. Adding an app to firmware/ without
adding it here is a hard error in T0 (see `check_manifest_covers_tree`), so the
matrix cannot silently drift out of sync with the tree.

Tiers, and what each does and does NOT prove:

  T0 build   — every app x board compiles via `make`, and the resulting sdkconfig
               carries a real country code. Cheap, no hardware, catches most
               port-forward breakage (a stack bump that removes a morselib symbol
               fails here). Proves NOTHING about runtime behaviour.

  T1 smoke   — flash + boot + the radio actually comes up (real chip id, real fw
               version, real MAC) with the right country. Needs hardware, one board
               at a time, no peer. Catches "the build is fine but the radio is dead"
               — the exact failure a bare-idf.py country of "??" produces. Proves
               nothing about any on-air feature.

  T2 on-air  — each feature has its own firmware/test-<feature>/ app that runs the
               feature and self-reports a machine-readable verdict on the console.
               Needs a real rig (multiple boards and/or Linux nodes). This is the only
               tier that proves a milestone claim still holds.

Expected values are sourced from the recorded milestones/worklogs, and each carries
its provenance so a reviewer can check the number rather than trust it.
"""

from __future__ import annotations

import os
import re
from dataclasses import dataclass, field
from typing import Optional

#: A well-formed MAC: six colon-separated hex octets. _mesh_mac/_mesh_ip only touch octets 0 and 5,
#: so a bad octet 1-4 derives a non-empty (wrong) value; validate the whole shape here.
_MAC_RE = re.compile(r"^([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}$")

# --------------------------------------------------------------------------
# Boards
# --------------------------------------------------------------------------

#: Board overlays under boards/<name>/. esp32s3.
BOARDS = ("proto1-fgh100m",)

#: Boards that are KNOWN BROKEN, with the reason. Builds for these are reported XFAIL
#: (counted + printed, but they do not gate the run) and XPASS if they unexpectedly
#: succeed — so a documented breakage stays visible without making `make test-t0`
#: permanently red, and the entry gets removed loudly the moment someone fixes it.
#:
#: Each entry is (reason, signature): the signature is a substring the recorded failure's
#: output must contain, so a known-broken board that later fails for a DIFFERENT reason
#: still reports a real FAIL instead of hiding under XFAIL.
#:
#: None today — the previously-broken `proto1` overlay (missing bcf_mf16858 BCF) was retired.
KNOWN_BROKEN_BOARDS: dict[str, tuple[str, str]] = {}


def known_broken(board: str) -> Optional[tuple[str, str]]:
    """(reason, signature) for a known-broken board, or None."""
    return KNOWN_BROKEN_BOARDS.get(board)

# --------------------------------------------------------------------------
# Bench identity — supplied by `make`, NEVER stored here.
# --------------------------------------------------------------------------
# Every physical fact about the bench is passed in from the Makefile (as env vars)
# and read here; nothing is hardcoded, so a different bench needs no source edit.
# Provide them per run, or `export` them once in your shell:
#
#   BENCH_BOARD   sdkconfig overlay for the ESP boards (e.g. proto1-fgh100m)
#   BOARD0_MAC    board0 efuse MAC   (the light-load / spare slot)
#   BOARD1_MAC    board1 efuse MAC   (the light-load / spare slot)
#   BOARD2_MAC    board2 efuse MAC   (the fully-wired DUT/relay/gate slot)
#   WIRED_BOARD   which logical board is fully wired (WAKE+BUSY) -- normally board2
#   LINUX_HOST    the Linux interop node's ssh host (e.g. chronite)
#   LINUX_MAC     that node's wlan1 MAC
#   LINUX_IP      that node's mesh IP (10.9.9.x)
#
# mesh_mac / mesh_ip / fully_wired / usb_bus_powered are DERIVED from these. Tiers that
# need the bench call require_bench() / require_linux(), which error clearly if a var is
# missing -- so the no-hardware tiers (t0, report) run with nothing set.


def _env(name: str) -> str:
    return (os.environ.get(name) or "").strip()


#: sdkconfig overlay for the ESP boards (make-supplied; empty until BENCH_BOARD is passed).
BENCH_BOARD = _env("BENCH_BOARD")


@dataclass(frozen=True)
class BenchBoard:
    """A physical ESP32 node on the bench.

    `efuse_mac` is the ONLY stable identifier — the /dev/ttyACM* numbers re-enumerate on
    every hotplug. Ports are resolved from /dev/serial/by-id/ via this MAC (common.resolve_port).
    """

    name: str
    efuse_mac: str
    #: Locally-administered MAC the apps derive (efuse mac[0] |= 0x02).
    mesh_mac: str
    mesh_ip: str
    #: True only for WIRED_BOARD: WAKE (GPIO2) + BUSY (GPIO5) wired to the MM6108. Those pins
    #: are the chip's PS / flow-control handshake, so any load / relay / power-save DUT role
    #: MUST use the wired board or the result is meaningless (has produced false bugs twice).
    fully_wired: bool
    #: The wired board's power comes from the PPK2 DUT rail, so it only enumerates while
    #: tools/ppk2_hold.py runs; the others are USB-bus-powered.
    usb_bus_powered: bool
    notes: str = ""


@dataclass(frozen=True)
class LinuxNode:
    """A Linux HaLow node usable as an interop peer (a T2 `linux:<host>` support role).

    `host` is the ONLY way the harness reaches it — resolved by `~/.ssh/config` (key auth,
    passwordless sudo), never a raw IP. `mesh_ip` is the data-plane address on wlan1 (10.9.9.N);
    `mesh_mac` is the node's wlan1 MAC as the ESP sees it. Supplied via make (LINUX_HOST/MAC/IP).
    """

    host: str
    mesh_ip: str
    mesh_mac: str        # wlan1 MAC; "" if unknown (read live at bring-up instead)
    role: str = ""


def _mesh_mac(efuse: str) -> str:
    """The locally-administered MAC the apps derive: efuse mac[0] |= 0x02, lowercased."""
    p = efuse.strip().lower().split(":")
    if len(p) != 6:
        return ""
    try:
        p[0] = f"{int(p[0], 16) | 0x02:02x}"
    except ValueError:
        return ""
    return ":".join(p)


def _mesh_ip(efuse: str) -> str:
    """The static mesh IP the apps derive: 10.9.9.<100 + (efuse mac[5] & 0x3f)>."""
    p = efuse.strip().split(":")
    if len(p) != 6:
        return ""
    try:
        return f"10.9.9.{100 + (int(p[5], 16) & 0x3f)}"
    except ValueError:
        return ""


_BOARD_MAC_ENV = {"board0": "BOARD0_MAC", "board1": "BOARD1_MAC", "board2": "BOARD2_MAC"}


def _build_bench() -> dict[str, BenchBoard]:
    """Build BENCH from the env vars make passes; {} unless BENCH_BOARD + all 3 MACs + WIRED_BOARD are set."""
    macs = {name: _env(var) for name, var in _BOARD_MAC_ENV.items()}
    wired = _env("WIRED_BOARD")
    if not _env("BENCH_BOARD") or not all(macs.values()) or not wired:
        return {}
    out: dict[str, BenchBoard] = {}
    for name, mac in macs.items():
        fw = (name == wired)
        out[name] = BenchBoard(
            name=name, efuse_mac=mac, mesh_mac=_mesh_mac(mac), mesh_ip=_mesh_ip(mac),
            fully_wired=fw, usb_bus_powered=not fw,
            notes=("The fully-wired ESP (WAKE+BUSY): the only load / relay / power-save DUT; "
                   "PPK2-powered, enumerates only while tools/ppk2_hold.py runs." if fw else
                   "WAKE+BUSY unwired. Light-load endpoint / spare ONLY. Never a relay or PS DUT."),
        )
    return out


def _build_linux() -> dict[str, LinuxNode]:
    """Build the Linux interop-peer registry from the env vars make passes; {} unless HOST + IP are set."""
    host, mac, ip = _env("LINUX_HOST"), _env("LINUX_MAC"), _env("LINUX_IP")
    if not host or not ip:
        return {}
    return {host: LinuxNode(host, ip, mac, "interop peer (passed via make)")}


#: Built from the make-supplied environment; empty when the bench vars aren't set. Tiers that need
#: the bench call require_bench() / require_linux() for a clear error rather than a bare KeyError.
BENCH: dict[str, BenchBoard] = _build_bench()
LINUX_NODES: dict[str, LinuxNode] = _build_linux()

#: The default Linux interop peer = whatever LINUX_HOST make passed.
DEFAULT_LINUX_PEER = _env("LINUX_HOST")


class BenchNotConfigured(SystemExit):
    """A clean exit (not a traceback) when a tier needs bench identity that make didn't pass."""


def _validate_bench(bench: dict[str, BenchBoard]) -> None:
    """Loud errors for a MALFORMED (not absent) bench config.

    Absence is handled by require_bench's "missing vars" path; this catches values that are
    present-but-wrong and would otherwise resolve to a silently-broken bench:
      - WIRED_BOARD naming a board that doesn't exist -> no fully-wired DUT/relay/PS slot;
      - a BOARDx_MAC that isn't a valid MAC -> empty derived mesh_mac/mesh_ip (never resolves);
      - two boards deriving the same mesh IP -> mesh/interop tests can't tell them apart.
    """
    problems: list[str] = []

    # WIRED_BOARD must name a real board, else `fully_wired` is False for all of them and there
    # is no relay / power-save / DUT slot (which has produced false bugs before).
    if not any(b.fully_wired for b in bench.values()):
        problems.append(
            f"WIRED_BOARD={_env('WIRED_BOARD')!r} is not one of {sorted(bench)} -- no board is "
            "fully wired, so there is no relay/power-save/DUT slot. Set WIRED_BOARD to a real board.")

    # A present-but-malformed MAC passes the non-empty check in _build_bench. Validate the FULL shape
    # here (not just the empty-derived mesh_mac/ip, which only catches a bad octet 0 or 5).
    for name in sorted(bench):
        b = bench[name]
        if not _MAC_RE.match(b.efuse_mac.strip()):
            problems.append(
                f"{_BOARD_MAC_ENV[name]}={b.efuse_mac!r} is not a valid MAC "
                "(need 6 colon-separated hex octets, e.g. e0:72:a1:f8:ef:a4).")

    # The apps derive 10.9.9.<100 + (mac[5] & 0x3f)>, so two boards whose last octet collides in
    # its low 6 bits map to the SAME IP and become indistinguishable on-air.
    by_ip: dict[str, list[str]] = {}
    for name in sorted(bench):
        ip = bench[name].mesh_ip
        if ip:
            by_ip.setdefault(ip, []).append(name)
    for ip, names in by_ip.items():
        if len(names) > 1:
            problems.append(
                f"{' and '.join(names)} both derive mesh IP {ip} (their efuse MACs share the low "
                "6 bits of the last octet); mesh/interop tests can't tell them apart -- use "
                "different boards for this run.")

    if problems:
        raise BenchNotConfigured(
            "bench config is malformed (values were passed but are wrong):\n"
            + "\n".join(f"  - {p}" for p in problems))


def require_bench() -> dict[str, BenchBoard]:
    """Return BENCH, or exit with a clear message naming the missing/malformed make vars."""
    if not BENCH:
        missing = [v for v in ("BENCH_BOARD", "BOARD0_MAC", "BOARD1_MAC", "BOARD2_MAC", "WIRED_BOARD")
                   if not _env(v)]
        raise BenchNotConfigured(
            "bench identity not provided. This tier needs the ESP boards -- pass them to make (or "
            "`export` them once), e.g.:\n"
            "  make <target> BENCH_BOARD=proto1-fgh100m BOARD0_MAC=<mac> BOARD1_MAC=<mac> "
            "BOARD2_MAC=<mac> WIRED_BOARD=board2\n"
            f"  missing: {', '.join(missing) or '(check the values)'}")
    _validate_bench(BENCH)
    return BENCH


def require_linux() -> dict[str, LinuxNode]:
    """Return LINUX_NODES, or exit with a clear message naming the missing make vars."""
    if not LINUX_NODES:
        missing = [v for v in ("LINUX_HOST", "LINUX_MAC", "LINUX_IP") if not _env(v)]
        raise BenchNotConfigured(
            "Linux interop node not provided. This tier needs a Linux peer -- pass it to make (or "
            "`export` it), e.g.:\n"
            "  make <target> LINUX_HOST=chronite LINUX_MAC=3c:22:7f:37:51:38 LINUX_IP=10.9.9.2\n"
            f"  missing: {', '.join(missing) or '(check the values)'}")
    return LINUX_NODES

#: The whole bench is pinned here. A mismatch reads as mystery breakage, so T1
#: asserts it rather than hoping (see memory: all Morse fw + Linux drivers matched).
EXPECTED_FW_VERSION = "1.17.8"
EXPECTED_CHIP_ID = "0x0306"
#: mm6108.bin sizes, for telling the versions apart from a dmesg line.
FW_SIZE_BY_VERSION = {480664: "1.17.8", 481040: "1.17.9"}
#: The pinned MM6108 firmware blob, for the T0 version-pin tripwire. The whole bench (ESP + Linux
#: nodes) runs this one version; a silent bump is the ~2x STA-PS regression the tp tier exists to
#: catch, so T0 asserts the exact blob BEFORE any board is flashed (docs/worklog/
#: 2026-07-17-powersave-test-cases-batch.md). size 480664 + sha256 = 1.17.8 (cross-checked here).
FW_BLOB_PATH = "vendor/morse-firmware/firmware/mm6108.bin"
EXPECTED_FW_SIZE = 480664
EXPECTED_FW_SHA256 = "ce2702b7fe0040d7a95a0ef859057b0114f021e21e38204a60fdd86f2a857728"

#: S1G channel 27 / 915.5 MHz on-air; `iw ... set freq 5560` in the Linux 5 GHz model.
RADIO_CHANNEL_S1G = 27
RADIO_FREQ_IW = 5560


# --------------------------------------------------------------------------
# Apps
# --------------------------------------------------------------------------


@dataclass(frozen=True)
class App:
    name: str
    #: Boards this app is built for in T0. Empty = excluded from the matrix.
    boards: tuple[str, ...] = BOARDS
    #: True if the app brings the radio up (T1 can assert radio bring-up on it).
    radio: bool = True
    #: True if the app drives the board into sleep/deep-sleep. These WEDGE THE USB
    #: (docs/reference/rimba-bench-devices.md:221-236) — flashing one to a board can
    #: cost a PPK2 power-cycle to recover, so T1 does not flash them by default.
    sleeps: bool = False
    excluded_reason: Optional[str] = None
    notes: str = ""


#: Every app under firmware/. Keep in sync with the tree — T0 enforces it.
APPS: tuple[App, ...] = (
    App("rimba-hello", radio=False, notes="Radio-free idle. The radio-silent resting state."),
    App("rimba-halow-scan", notes="Scan only."),
    App("rimba-halow-ap", notes="SoftAP. CONFIG_HALOW_AP_MODE=y, max 255 STAs, PSRAM sta data."),
    App("rimba-halow-sta", notes="STA example: join a HaLow SoftAP (SAE) + ping it. "
        "(The C6-triggered PS-ladder rig moved to test-power.)"),
    App("rimba-halow-ap-perf", notes="AP side of the iperf throughput rig."),
    App("rimba-halow-sta-perf", notes="STA side of the iperf throughput rig."),
    App("rimba-halow-ibss", notes="IBSS / ad-hoc (RISK-01)."),
    App("rimba-halow-mesh", notes="802.11s mesh point (SAE+AMPE)."),
    App("rimba-halow-mesh-ap", notes="Mesh + AP concurrency on one MM6108 (the mesh-gate)."),
    App("rimba-twt-assoc", notes="TWT power-save example: assoc-embedded TWT + doze (the universal path)."),
    App("rimba-deepsleep-cycle", sleeps=True,
        notes="Deep-sleep battery-leaf example (connect -> deep-sleep -> wake -> reconnect). The "
              "dscycle harness fixture is the test-deepsleep-cycle clone."),
    # ---- regtest fixtures: harness resting-state + the power/deep-sleep DUTs ----
    # Not T2 feature tests; the harness FLASHES these as fixtures. Under the split rule
    # (the harness references ONLY test-* apps), the load-bearing fixtures live here;
    # their clean user-facing counterparts stay as rimba-* examples.
    App("test-idle", radio=False,
        notes="Radio-silent resting-state fixture (IDLE_APP). Flashed to every board after every "
              "hardware test (common.go_radio_silent). Clone of rimba-hello, trimmed to the idle loop."),
    App("test-deepsleep-cycle", sleeps=True,
        notes="dscycle DUT: a deep-sleep leaf woken by test-c6-trigger's D5 pulse; re-associates and "
              "logs 'RECONNECTED in <ms>' (scraped by dscycle.py). Verbatim clone of the "
              "rimba-deepsleep-cycle example, with the bench-rig hooks retained."),
    App(
        "test-c6-trigger",
        boards=(),
        radio=False,
        excluded_reason=(
            "Target esp32c6 + standalone IDF project; the repo Makefile is esp32s3-only "
            "(BOARD overlays both set CONFIG_IDF_TARGET=esp32s3). Built by hand per "
            "docs/reference/rimba-bench-devices.md. Excluded from the APP x BOARD "
            "matrix by design, not by oversight."
        ),
        notes="ESP32-C6 D5 trigger fixture for the tp (test-power) + dscycle "
              "(test-deepsleep-cycle) tiers: drives board2's D5 trigger / flash-hold.",
    ),
    # ---- T2 feature tests (firmware/test-*) ----------------------------
    # These are built by T0 like any other app, so a regtest app that stops compiling
    # is caught by the cheap tier rather than on the bench.
    App("test-swccmp", radio=False,
        notes="T2: RFC-3610 CCM KAT on target. No radio, no peer, deterministic."),
    App("test-apsta-ap",
        notes="T2 ap-sta-ping: the AP (support) role. SoftAP + static IP; needs "
              "CONFIG_HALOW_AP_MODE=y (its sdkconfig.defaults)."),
    App("test-apsta-sta",
        notes="T2 ap-sta-ping: the STA (reporter) role."),
    App("test-mesh-peering",
        notes="T2 mesh-peering: symmetric mesh node (both peers run it). Needs "
              "CONFIG_HALOW_AP_MODE=y. Reporter polls mmwlan_mesh_peer_count()."),
    App("test-twt-sta",
        notes="T2 twt: STA requester+reporter. AP role = test-apsta-ap (TWT responder "
              "default-on). Polls mmwlan_twt_agreement_installed() -> INSTALLED."),
    App("test-twt-assoc-sta",
        notes="T2 twt-assoc: STA reporter for the ASSOC-EMBEDDED TWT path (add_configuration before "
              "connect -> TWT in the assoc IEs; the universal path both APs honour). Scans flows "
              "0..3 for INSTALLED."),
    App("test-ampdu-cap",
        notes="T2 ampdu-cap: single board, mesh vif up, reads "
              "mmwlan_ampdu_capability_advertised(). Needs CONFIG_HALOW_AP_MODE=y."),
    App("test-ibss",
        notes="T2 ibss: symmetric IBSS node (both run it; creator pinned to board0's MAC). "
              "Reporter polls mmwlan_ibss_peer_count()==1. Needs CONFIG_HALOW_AP_MODE=y."),
    App("test-mesh-relay",
        notes="T2 mesh-relay: symmetric 3-node (origin/relay/dest by MAC; forced-line allowlist). "
              "RELAY=board2 (require_wired). Origin reports. Needs CONFIG_HALOW_AP_MODE=y."),
    App("test-mesh-large-frame",
        notes="T2 mesh-large-frame: test-mesh-relay's forced line with a LARGE ICMP payload so the FW "
              "fragments the encrypted mesh frame -> exercises the defrag-before-decrypt RX path. "
              "RELAY=board2 (require_wired). Origin reports. Needs CONFIG_HALOW_AP_MODE=y."),
    App("test-mesh-leaf",
        notes="T2 mesh-leaf: test-mesh-relay's forced line but the relay calls "
              "mmwlan_mesh_set_multihop(false) (leaf/single-hop). Origin inverts: PASS iff peering "
              "survives AND forwarding is blocked (0 replies). RELAY=board2 (require_wired). "
              "Origin reports. Needs CONFIG_HALOW_AP_MODE=y."),
    App("test-mesh-relay-nocrash",
        notes="T2 mesh-relay-nocrash: forced line, but the RELAY is the reporter (origin+dest are "
              "support; origin drives a long ping burst). Relay forwards, then asserts its "
              "hw_restart_counter did NOT climb across the window (the interrupt-WDT crash). "
              "RELAY=board2 (require_wired). Needs CONFIG_HALOW_AP_MODE=y."),
    App("test-mesh-ap-gate",
        notes="T2 mesh-ap: the GATE support role (mesh+AP+ip_forward on one radio). board2. "
              "Needs CONFIG_HALOW_AP_MODE=y + CONFIG_LWIP_IP_FORWARD=y."),
    App("test-mesh-ap-peer",
        notes="T2 mesh-ap: the far mesh node (board1, 10.9.9.100); return route via gate 10.9.9.108. "
              "Needs CONFIG_HALOW_AP_MODE=y."),
    App("test-mesh-ap-sta",
        notes="T2 mesh-ap: STA reporter (board0) behind the AP; pings 10.9.9.100, asserts ttl=63."),
    App("test-mesh-linux",
        notes="T2 mesh-linux: ESP<->Linux mesh interop reporter. Mesh ID rimba-smesh (matches the "
              "Linux wpa-smesh.conf); peers with + pings chronite. Needs CONFIG_HALOW_AP_MODE=y."),
    # ---- mesh-gate (802.11s gate discovery + Address-Extension datapath) ----
    # The S1-S5 dev fixtures, now wired into T2 (mesh-gate-* T2Tests). Case B reuses
    # test-mesh-gate-relay as a self-reporter; S1/S2 byte-diff (test-mesh-gate) is the
    # deferred Linux tier (S6). See docs/mesh-ap/rimba-mesh-ap-s6-interop-plan.md.
    App("test-mesh-gate",
        notes="mesh-gate S1: lone PROACTIVE_RANN gate emitter (flags=0x01 IS_GATE, 5000ms). No on-box "
              "verdict -- the check is an off-line RANN byte-diff vs a live Linux gate (Case E / S6). "
              "Needs CONFIG_HALOW_AP_MODE=y."),
    App("test-mesh-gate-rx",
        notes="mesh-gate S2/S3: mesh node -- RANN RX + re-flood + gate learner; polls "
              "mmwlan_mesh_gate_count (known_gates>0) + mmwlan_mesh_ae_rx_probe (AE eaddrs). GATE=1 makes "
              "it a gate emitter; MESH_ID= sets the mesh. Needs CONFIG_HALOW_AP_MODE=y."),
    App("test-mesh-gate-fwd",
        notes="mesh-gate S4b: send_to_gates TX fallback -- learns a gate, then sends an AE frame via it "
              "every 3s. Paired with a gate's AE-rx probe for the full proof. Needs CONFIG_HALOW_AP_MODE=y."),
    App("test-mesh-gate-relay",
        notes="mesh-gate S4c (Case B reporter): symmetric 3-role NODE<->RELAY<->GATE (role-by-MAC, "
              "forced-line allowlist). GATE asserts ae_rx>0 with eaddr1/eaddr2 preserved through the "
              "relay -- covers S2-learn + S3 + S4a + S4b + S4c. RELAY=board2 (require_wired; does the "
              "forwarding), GATE=board0 (the reporter). Needs CONFIG_HALOW_AP_MODE=y."),
    App("test-mesh-ae",
        notes="mesh-gate S3/S5b: AE injector -- originates 6-address AE_A5_A6 frames (LINUX_MAC/EADDR1/"
              "MESH_ID configurable). Verdict is receiver-side (Linux `iw mpp dump` or a paired "
              "test-mesh-gate-rx AE probe). Needs CONFIG_HALOW_AP_MODE=y."),
    App("test-mesh-gate-sta",
        notes="mesh-gate-bridge (Case C) STA REPORTER: DHCP client behind the retire-L3 gate's AP; pings "
              "a mesh node (10.9.9.100) zero-config across the flat-subnet L2 bridge. PASS iff associated + "
              "leased + >=6 replies + ttl==64 (pure L2 bridge, no ip_forward). Gate = the test-mesh-gate-ap "
              "fixture; proves retire-L3 + round-trip + S5b/S5c + B1 + proxy-ARP."),
    App("test-mesh-gate-node",
        notes="mesh-gate endpoint mesh-node: DEFAULT = pinger REPORTER (mesh-gate-b2 Case D, pings a silent "
              "static AP-client 10.9.9.50; PASS iff >=15/30 -- proxy-ARP-backed reliability, not the lossy "
              "raw B2 broadcast). NO_PING=1 = a plain static mesh RESPONDER (mesh-gate-bridge Case C support). "
              "Needs CONFIG_HALOW_AP_MODE=y."),
    App("test-mesh-gate-ap",
        notes="mesh-gate GATE fixture (support role for both mesh-gate-bridge + mesh-gate-b2): the retire-L3 "
              "mesh + co-channel SoftAP L2 bridge + proxy-ARP + DHCP on one MM6108 (a test-* clone of the "
              "rimba-halow-mesh-ap example). board2, up_marker 'L2 bridge ready'. Needs CONFIG_HALOW_AP_MODE=y."),
    # ---- tp (power) tier (firmware/test-power) -------------------------
    App("test-power",
        notes="tp power tier: the C6-triggered 4-tier PS ladder DUT (board2, require_wired). The "
              "PPK2 meters the rail; tp_power.py segments the current stream + scores Dyn-PS + "
              "WNM against calibrated bands. STA + TEST| markers; no RESULT (verdict is host-side)."),
)

#: test-* apps that are DEFINED (rig + expectations, in t2_tests.py) and have a README,
#: but whose firmware is not written yet. Listed explicitly so the gap is visible in the
#: suite's own output rather than being discoverable only by noticing an absence.
#: Honest-partial beats fake-complete.
T2_NOT_IMPLEMENTED: dict[str, str] = {}

APPS_BY_NAME = {a.name: a for a in APPS}

#: Faithful test-* fixtures whose app_main.c BODY must stay byte-for-byte identical to a production
#: rimba-* example. The leading /* */ banner is intentionally per-app (it names the fixture) and is
#: EXCLUDED from the compare; everything from the first `#include` to EOF must match. T0 asserts this
#: (t0_build._check_clone_mirrors) so a clone can't silently drift from its source -- a patch to one
#: app_main.c that forgets the other would leave the fixture testing stale code. ONLY verbatim body
#: clones belong here (an explicit allowlist): test-mesh-gate-ap is a byte clone of the shipped gate;
#: DERIVED fixtures (test-idle, test-deepsleep-cycle) are deliberately absent. Adding a clone later =
#: one new row. clone -> (source_app, (repo-relative files under firmware/<app>/, ...)).
CLONE_MIRRORS: dict[str, tuple[str, tuple[str, ...]]] = {
    "test-mesh-gate-ap": ("rimba-halow-mesh-ap", ("main/app_main.c",)),
}

#: Apps that participate in the T0 matrix.
T0_APPS = tuple(a for a in APPS if a.boards)

#: The radio-silent resting app (a regtest fixture). Flashed to every ESP after every bench test.
IDLE_APP = "test-idle"


# --------------------------------------------------------------------------
# T2 — on-air feature tests
# --------------------------------------------------------------------------


@dataclass(frozen=True)
class Expectation:
    """A recorded value a T2 test asserts against, WITH its provenance.

    `noisy` is the important field. An RF-dependent number (throughput, loss %,
    RSSI) makes a flaky assertion — the bench's own docs record RX overload at close
    range and a power-marginal node. Noisy expectations are asserted as a wide floor
    or via a binary proxy, never as an equality.
    """

    metric: str
    value: str
    source: str
    noisy: bool
    #: What the test actually asserts (may be weaker than `value` when noisy).
    assertion: str


@dataclass(frozen=True)
class Role:
    """One node in a multi-role T2 rig, for the orchestrator.

    A test with `roles` set is AUTOMATABLE end-to-end: the orchestrator resolves each
    role to a device, flashes its app (ESP roles) or brings it up over ssh (Linux roles),
    boots them in order, and reads the REPORTER role's TEST| verdict.
    """

    name: str
    #: "board0" | "board1" | "board2" | "any-esp" (first present ESP) | "linux:<host>".
    device: str
    #: For an ESP role: the firmware app to flash. For a Linux role: unused (see linux_setup).
    app: Optional[str] = None
    #: Exactly one role per test is the reporter — its TEST|RESULT is the test verdict.
    reporter: bool = False
    #: Support roles boot first; after flashing one, wait up to this long so it is up before
    #: the reporter starts (e.g. the AP must beacon before the STA associates).
    boot_wait_s: float = 8.0
    #: Optional console substring proving a support role actually came up (e.g. the AP's
    #: "static IP" line). If set and not seen within boot_wait_s, the orchestrator fails the
    #: test early with "the <role> role never started" instead of a confusing reporter error.
    up_marker: Optional[str] = None
    #: True => must be board2 (the only fully-wired ESP). The orchestrator refuses to run a
    #: load/relay/PS role on board0/board1 (unwired WAKE/BUSY) rather than produce a false bug.
    require_wired: bool = False
    #: For a Linux role: a key into the runbook the orchestrator knows how to drive
    #: (e.g. "mesh-peer"). None for ESP roles.
    linux_setup: Optional[str] = None
    #: For a SYMMETRIC ESP app (every board runs the same firmware and self-selects its role by
    #: comparing its own MAC to a set of known peers — e.g. the 3-node relay line): the name of
    #: the build-time MAC variable this role's assigned board should feed. The orchestrator
    #: collects every role's (build_mac_var -> assigned-board mesh_mac) and passes the whole set
    #: to EVERY flash of that app, so all boards agree on the topology without a source edit.
    #: Compiled as TEST_<build_mac_var> (see the app's CMakeLists.txt). None for asymmetric roles.
    build_mac_var: Optional[str] = None
    #: FIXED per-role build vars, as (make-shorthand, value) pairs threaded verbatim to this role's flash
    #: (e.g. (("STA_IP","10.9.9.50"),("NO_PING","1")) to pin a production STA as a static responder). The
    #: shorthand must be one the root Makefile threads into a -D (see IDF_EXTRA_D). A tuple (not a dict) so
    #: the frozen dataclass stays hashable. NOTE: applied per-APP, so two roles sharing an app share vars.
    build_vars: tuple[tuple[str, str], ...] = ()


@dataclass(frozen=True)
class T2Test:
    """One T2 feature test: a rig + the recorded values it asserts against."""

    slug: str
    title: str
    #: Human-readable rig (role -> device description). Always present, for --dry-run + READMEs.
    rig: dict[str, str]
    what_it_proves: str
    what_it_does_not_prove: str
    #: One-line "value needed to pass" — the structural gate, in plain words, for the report's
    #: "needs to pass" column. Authored (not derived) because a test has several expectations and
    #: only the reader knows which one is THE gate. Kept terse; the full expectations carry the rest.
    pass_if: str = ""
    expectations: tuple[Expectation, ...] = field(default_factory=tuple)
    #: Set when the test cannot be fully automated; explains the concrete blocker.
    manual_steps: Optional[str] = None
    #: Structured roles the orchestrator can drive. When set (and every referenced app
    #: exists), the test is automated; when empty, it is defined-but-not-automated.
    roles: tuple[Role, ...] = field(default_factory=tuple)

    @property
    def app(self) -> str:
        """The single-app name for a 1-board test (legacy convenience)."""
        return f"test-{self.slug}"

    @property
    def automated(self) -> bool:
        return bool(self.roles)

    @property
    def reporter_role(self) -> Optional["Role"]:
        for r in self.roles:
            if r.reporter:
                return r
        return None


def t2_tests() -> tuple[T2Test, ...]:
    """The T2 catalogue. Defined in t2_tests.py to keep this file readable."""
    from .t2_tests import T2_TESTS

    return T2_TESTS


def check_manifest_covers_tree(firmware_dir) -> list[str]:
    """Return a list of drift errors between firmware/ and APPS.

    Called by T0. An app added to the tree but not the manifest is a silent hole in
    the regression matrix, so it fails the run rather than warning.
    """
    from pathlib import Path

    firmware_dir = Path(firmware_dir)
    on_disk = {
        p.name
        for p in firmware_dir.iterdir()
        if p.is_dir() and (p / "CMakeLists.txt").exists()
    }
    known = set(APPS_BY_NAME)
    errors = []
    for missing in sorted(on_disk - known):
        errors.append(
            f"app '{missing}' exists in firmware/ but is not in tools/regtest/manifest.py APPS "
            f"-- it is therefore untested. Add it (with boards=() + excluded_reason if it "
            f"should not be built)."
        )
    for gone in sorted(known - on_disk):
        errors.append(
            f"app '{gone}' is in tools/regtest/manifest.py APPS but not in firmware/ -- "
            f"stale manifest entry."
        )
    return errors


# --------------------------------------------------------------------------
# tp — the PPK2 power-save tier
# --------------------------------------------------------------------------
#
# A power verdict comes from a HOST-side PPK2 current stream the firmware cannot see, so it can
# never be a firmware TEST|RESULT (T2's model). The `tp` tier owns the PPK2, runs the
# test-power C6-triggered 4-tier ladder on board2, segments the current stream by the DUT's
# TEST| phase markers, and scores a GROSS current regression (the kind the fw-1.17.9 ~2x PS
# regression was) while ALWAYS recording the raw mA. Doze DEPTH is a benchmark, not a pass/fail
# (docs/reference/rimba-halow-ps-esp32-vs-linux-ap.md) -- so the gate is deliberately wide:
# PASS below a comfortable band, FAIL only on a gross multiple, INCONCLUSIVE in between.


@dataclass(frozen=True)
class PowerTier:
    key: str            # phase key, matches the DUT's "phase=N <key> start" marker
    phase: int          # 1..4, the ladder order
    label: str
    #: Scored tiers gate; unscored ones are measured + recorded only (No-PS is the validity
    #: anchor; TWT is AP-dependent -- a Linux AP ignores the mid-session setup -- so recorded).
    scored: bool
    provenance: str


POWER_TIERS: tuple[PowerTier, ...] = (
    PowerTier("no-ps", 1, "No-PS (radio always on)", False,
              "validity anchor: proves associated + PS-on + not RX-overloaded; ~65 mA, "
              "unchanged by the PS regression (radio always on)"),
    PowerTier("dyn-ps", 2, "Dynamic PS", True,
              "docs/reference/rimba-halow-ps-esp32-vs-linux-ap.md:41 (Linux AP 9.1 -> 20.2 mA "
              "on 1.17.9); ESP AP good ~15.3"),
    PowerTier("twt", 3, "TWT (10 s SP)", False,
              "recorded-not-scored: a Linux hostapd ignores the mid-session TWT setup -> STA "
              "falls back to dyn-PS (~9.9 mA), so no clean good/bad (ref doc :242)"),
    PowerTier("wnm-powerdown", 4, "WNM + chip-powerdown", True,
              "docs/reference/rimba-halow-ps-esp32-vs-linux-ap.md:50 (Linux AP 5.1 -> 17.5 mA "
              "on 1.17.9); the deepest stay-associated tier"),
)

#: No-PS validity window (mA). Outside it, the WHOLE run is INCONCLUSIVE (unassociated / PS not
#: actually off / RX-overloaded) rather than a per-tier FAIL -- a bad link must not read as a
#: code regression. ~65 mA nominal (ref doc); wide margin for AP/thermal variation.
POWER_NOPS_VALID_MA: tuple[float, float] = (55.0, 72.0)

#: Scored-tier bands per AP, (pass_max_ma, fail_min_ma): PASS <= pass_max; FAIL >= fail_min;
#: between = INCONCLUSIVE; drawing BELOW pass_max is never a FAIL (a possible improvement, reported
#: with its number). Wide by design (gross-multiple gate).
#:
#: CALIBRATED on THIS rig at fw 1.17.8, 2026-07-16 (docs/worklog/2026-07-16-ppk2-power-regression-
#: tier.md), 3 runs/AP. The rig reads ~1.6-2x the documented reference on every doze tier -- a
#: close-bench RX-OVERLOAD offset (the DUT TX-caps to 1 dBm but the AP transmits at full power from
#: cm away, so the radio keeps waking on retries). The offset is REPEATABLE, so pass_max sits ~1.3x
#: above the noisiest good run and fail_min at ~1.9x the baseline -- every 1.17.8 run PASSes with
#: margin, and a 1.17.9-style ~2-3x regression FAILs (a milder ~1.5x lands INCONCLUSIVE, honest per
#: the doctrine). Observed 1.17.8 (median mA): ESP Dyn-PS 21.4-25.5 / WNM 13.4-18.3; Linux Dyn-PS
#: 21.8-23.1 / WNM 14.3-15.8. v2 (backlog): cap the AP TX for reference-like numbers + a tighter
#: recalibration -- see the worklog "harden later".
POWER_BANDS: dict[str, dict[str, tuple[float, float]]] = {
    # Linux hostapd_s1g AP -- the authoritative reference; tighter run-to-run spread on this rig.
    "linux": {"dyn-ps": (30.0, 40.0), "wnm-powerdown": (21.0, 28.0)},
    # ESP32 SoftAP -- wider spread (a warmup excursion to 25.5/18.3), so wider bands.
    "esp":   {"dyn-ps": (32.0, 42.0), "wnm-powerdown": (24.0, 32.0)},
}
POWER_BANDS_CALIBRATED = True   # locked on-rig at 1.17.8, 2026-07-16 (RX-overload-offset baseline)

#: The host-LIGHT-SLEEP variant bands (tp --light-sleep, DUT built HOST_LIGHT_SLEEP=1). At DTIM1 each
#: radio wake pays a full PLL relock, so light-sleep BACKFIRES on the shallow doze tiers. In the
#: CLEAN reference (TX-capped AP) this is a STRONGER secondary gate (LS WNM 1.17.8 ~4 mA -> 1.17.9
#: ~30 mA, ~7x); but CALIBRATED on THIS rig 2026-07-17 (3 runs/AP, docs/worklog/
#: 2026-07-17-powersave-test-cases-batch.md) the close-bench RX-OVERLOAD masks the LS win -- LS
#: 1.17.8 already reads ~25.8 mA (Dyn-PS) / ~22-25 (WNM), so the multiple vs a 1.17.9-style ~30 mA is
#: no bigger than the host-awake gate. So these bands catch a GROSS regression (like POWER_BANDS), and
#: the LS variant becomes the intended stronger gate only under v2 (cap the AP TX). Both APs read the
#: same LS numbers, so the bands are identical. Same (pass_max, fail_min) semantics as POWER_BANDS.
POWER_BANDS_LS: dict[str, dict[str, tuple[float, float]]] = {
    "linux": {"dyn-ps": (35.0, 48.0), "wnm-powerdown": (35.0, 46.0)},
    "esp":   {"dyn-ps": (35.0, 48.0), "wnm-powerdown": (35.0, 46.0)},
}
POWER_BANDS_LS_CALIBRATED = True   # locked on-rig at 1.17.8, 2026-07-17 (RX-overload-offset baseline)

#: The tp rig, referenced by tp_power.py (kept here as the single source of truth).
POWER_DUT_APP = "test-power"          # the ladder DUT; board2, require_wired
POWER_DUT_BOARD = "board2"
POWER_ESP_AP_APP = "test-apsta-ap"    # ESP SoftAP support role
POWER_ESP_AP_BOARD = "board0"            # a light-load AP role -> unwired board0 is fine
POWER_ESP_AP_MARKER = "ap-ready"         # test-apsta-ap's up-marker
POWER_LINUX_AP_HOST = DEFAULT_LINUX_PEER  # the make-supplied LINUX_HOST; hostapd_s1g AP (linux_peer.bring_up_ap)
POWER_C6_TRIGGER_PERIOD_S = 30           # the C6 free-runs a LOW pulse this often (test-c6-trigger)
POWER_PHASE_S = 18                       # seconds per ladder tier (test-power PHASE_S)
