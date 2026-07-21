"""T2 test definitions — one firmware/test-<slug>/ app per feature.

Expected values are taken from the recorded milestones/worklogs and each carries its
provenance so a reviewer can check the number instead of trusting it.

THE CENTRAL DESIGN RULE: assert the STRUCTURAL fact, not the RF number.

The bench's own docs are emphatic that RF numbers here are noisy:
  - close nodes at 27 dBm read RSSI ~ -3 dBm = RX OVERLOAD -> high loss + retries
    (docs/worklog/... ; memory: bench-halow-rx-overload)
  - chronosalt is POWER-MARGINAL and reboots on radio-up
    (docs/reference/rimba-bench-devices.md:66)
  - the mesh throughput ceiling is now "the bench RF link, not the code"
    (docs/rimba-todo.md)
  - the recorded numbers themselves include known single-packet misses (IBSS "4/5" on
    .86, AP "7/8 then 5/5") -- i.e. a hard equality assert on those WOULD have flaked
    on the very run that produced them.

So a test asserts "the mesh peered and forwarded" (binary, structural, reproducible),
never "throughput was 1.27 Mbps" (RF-bound, and a regression suite that cries wolf on
a fading link gets switched off). Where only a noisy number exists, the test asserts a
wide floor and reports the observed value, or returns INCONCLUSIVE rather than FAIL.
"""

from __future__ import annotations

from .manifest import Expectation, Role, T2Test

# ---------------------------------------------------------------------------
# Deterministic tests — no radio, or no peer. These are the suite's backbone:
# they cannot be blamed on the RF link, so a failure is unambiguously the code.
# ---------------------------------------------------------------------------

SWCCMP = T2Test(
    slug="swccmp",
    pass_if="selftest rc == 0 (RFC-3610 KAT, exact)",
    title="Host SW-CCMP correctness (RFC-3610 KAT) on target",
    rig={"dut": "any single board"},
    what_it_proves=(
        "The bulk-DMA AES-CCM that the mesh host-crypto datapath depends on produces the "
        "exact RFC-3610 Packet-Vector-1 ciphertext + MIC and round-trips through decrypt, "
        "on real ESP32-S3 silicon with the real mbedtls/HW-AES backend. That byte-exactness "
        "is what makes our mesh MICs interoperate with Linux/mac80211."
    ),
    what_it_does_not_prove=(
        "Any framing. The KAT covers the CCM primitive only -- not the 802.11 AAD/nonce "
        "construction (mesh_ccmp_aad_nonce), not defrag-before-decrypt ordering, not replay "
        "protection. Those need real frames -- see test-mesh-relay."
    ),
    expectations=(
        Expectation(
            metric="esp_mesh_ccm_selftest() return code",
            value="0",
            source="components/halow/.../umac/supplicant_shim/ccmp.c:141-155 "
                   "(RFC-3610 Packet-Vector-#1 KAT + decrypt round-trip)",
            noisy=False,
            assertion="rc == 0, exactly. A known-answer test has one right answer.",
        ),
    ),
)

AMPDU_CAP = T2Test(
    slug="ampdu-cap",
    pass_if="mesh A-MPDU capability bit present",
    title="Firmware advertises the mesh A-MPDU capability (S0a)",
    rig={"dut": "board2 (fully wired)"},
    what_it_proves=(
        "The MM6108 firmware still reports the mesh AMPDU capability the S0 spike proved on "
        "2026-07-11 -- i.e. the capability gate a stack bump could silently close is still open."
    ),
    what_it_does_not_prove=(
        "That aggregation actually happens on the air, or any throughput figure. A-MPDU is "
        "FW-assembled; the capability bit is necessary, not sufficient. Real aggregation "
        "needs a peer + a capture (test-mesh-relay + the chronium monitor)."
    ),
    expectations=(
        Expectation(
            metric="FW mesh AMPDU capability advertised",
            value="present",
            source="docs/worklog/2026-07-11-mesh-ampdu-s0-fw-capability-spike.md (S0a bench GO)",
            noisy=False,
            assertion="capability bit present -- binary, deterministic, no peer needed.",
        ),
        Expectation(
            metric="single-hop mesh throughput after A-MPDU",
            value="~0.2 -> 1.27 Mbps",
            source="docs/worklog/2026-07-11-mesh-ampdu-s0-fw-capability-spike.md (S0b)",
            noisy=True,
            assertion="NOT asserted by this test. RF-bound: the recorded ceiling is the bench "
                      "link (RX overload at close range), not the code. Throughput belongs in a "
                      "benchmark, not a pass/fail gate.",
        ),
    ),
)

# ---------------------------------------------------------------------------
# On-air tests — need a real rig.
# ---------------------------------------------------------------------------

AP_STA_PING = T2Test(
    slug="ap-sta-ping",
    pass_if="STA associates + >=1 ping reply, 0 crashes",
    title="AP <-> STA association + ping",
    rig={"ap": "board0 — test-apsta-ap (SoftAP, light load OK)",
         "sta": "board1 — test-apsta-sta (reporter)"},
    roles=(
        # AP support role. A dedicated test-* app (every app a T2 test flashes is
        # test-*): SoftAP + static IP 192.168.12.1 + a TEST| "ap-ready" up-marker.
        # Boots first; light load, so board0 (unwired WAKE/BUSY) is fine here.
        Role(name="ap", device="board0", app="test-apsta-ap", boot_wait_s=25.0,
             up_marker="ap-ready"),
        # STA reporter: associates, pins 192.168.12.2, pings the AP, emits the verdict.
        Role(name="sta", device="board1", app="test-apsta-sta", reporter=True),
    ),
    what_it_proves="An ESP SoftAP accepts an ESP STA association and IP data flows both ways.",
    what_it_does_not_prove=(
        "Throughput, power-save behaviour, or the AID>=64 ceiling (that needs 64+ associated "
        "STAs, which a 3-board bench cannot produce -- recorded as unverifiable, not skipped)."
    ),
    expectations=(
        Expectation(
            metric="association + ping replies",
            value="0 timeouts (33/33; AP-stress 600/600 at 0% loss over 120 s)",
            source="docs/worklog/2026-06-23-regression-stress-test.md:21,24",
            noisy=False,
            assertion="association reached AND >=1 reply AND 0 crashes. The '0 timeouts' "
                      "property is structural on a healthy bench; the exact count is "
                      "window-length-dependent so it is reported, not asserted.",
        ),
        Expectation(
            metric="round-trip time",
            value="~12 ms (range ~11-27 ms; one recorded run saw 7/8 then 5/5)",
            source="docs/worklog/2026-06-18-halow-ap-sta-ping.md:59-63; "
                   "docs/worklog/2026-06-23-ap-multinode-twt-hwtest.md:75",
            noisy=True,
            assertion="reported, never asserted. The source run itself recorded a "
                      "single-packet miss (7/8) -- a hard equality assert would have flaked "
                      "on the run that produced the number.",
        ),
    ),
)

IBSS = T2Test(
    slug="ibss",
    pass_if="CREATE ok + exactly 1 peer, 0 phantom",
    title="IBSS join/adopt + peer-table correctness",
    rig={"support": "board0 — test-ibss (creator, pinned by MAC)",
         "reporter": "board1 — test-ibss (joiner; polls the peer count)"},
    roles=(
        # Symmetric: both nodes run the SAME IBSS app. The app self-selects CREATE vs JOIN by
        # its own (efuse-derived) MAC -- board0 is the pinned creator, so it comes up first as
        # the support role and CREATEs the cell; board1 JOINs and reports. Light load, so
        # board0/board1 are fine. The reporter asserts EXACTLY 1 peer record (0-phantom check).
        Role(name="support", device="board0", app="test-ibss",
             boot_wait_s=30.0, up_marker="ibss-up"),
        Role(name="reporter", device="board1", app="test-ibss", reporter=True),
    ),
    what_it_proves=(
        "IBSS_CONFIG(CREATE) succeeds and a node forms exactly the right peer records with no "
        "phantom entries -- the structural core of the IBSS port. (Exactly-1-peer on a 2-node "
        "cell IS the 0-phantom check.)"
    ),
    what_it_does_not_prove=(
        "On-air frame equivalence with Linux (I.4 was never done -- the monitor was compiled "
        "out at the time; docs/ibss/rimba-ibss-test-plan.md:172), nor IBSS throughput "
        "(P1.1-P1.3 have no recorded number at all -- 'numbers TODO', :285)."
    ),
    expectations=(
        Expectation(
            metric="IBSS_CONFIG(CREATE) status",
            value="0",
            source="docs/ibss/rimba-ibss-milestones.md:110-116 (EEXIST -17 root-fixed)",
            noisy=False,
            assertion="== 0, exactly.",
        ),
        Expectation(
            metric="peer records per node",
            value="exactly 2 (3-node cell), AIDs 1 & 2, no self/BSSID/bogus entries",
            source="docs/ibss/rimba-ibss-test-plan.md:105-109",
            noisy=False,
            assertion="exact structural equality -- the strongest IBSS assert available. "
                      "Independent of RF quality: a fading link changes timing, not topology.",
        ),
        Expectation(
            metric="phantom peers",
            value="0 (after the divergence-17 fix)",
            source="docs/ibss/rimba-ibss-test-plan.md:180-181",
            noisy=False,
            assertion="== 0, exactly.",
        ),
        Expectation(
            metric="xpeer_collisions ACCEPTED",
            value="208 (over 9400 frames)",
            source="docs/ibss/rimba-ibss-test-plan.md:116-127",
            noisy=True,
            assertion="assert > 0 (the mechanism is alive), NEVER == 208. The exact count is "
                      "load-window-dependent.",
        ),
    ),
)

TWT = T2Test(
    slug="twt",
    pass_if="TWT agreement INSTALLED + STA stays associated",
    title="TWT responder (action frame) — agreement established",
    rig={"ap": "board0 — test-apsta-ap (SoftAP, TWT responder default-on)",
         "sta": "board1 — test-twt-sta (requester + reporter)"},
    roles=(
        # AP support role reuses test-apsta-ap: an AP vif is a TWT responder by default
        # (umac_interface.c:145). Same SoftAP the ap-sta-ping test uses.
        Role(name="ap", device="board0", app="test-apsta-ap", boot_wait_s=25.0,
             up_marker="ap-ready"),
        # STA reporter: associates, sends a TWT Setup Request, polls the agreement -> INSTALLED.
        Role(name="sta", device="board1", app="test-twt-sta", reporter=True),
    ),
    what_it_proves=(
        "The ESP SoftAP accepts a TWT Setup Request action frame and an agreement reaches "
        "INSTALLED (the action-frame TWT path, PR teapotlaboratories/rimba#15)."
    ),
    what_it_does_not_prove=(
        "The power saving itself (that needs the PPK2 current ladder, not a pass/fail test), "
        "nor Linux-STA-as-requester interop (recorded as blocked by PMF -- "
        "docs/worklog/2026-06-24-twt-action-frame.md:94-96)."
    ),
    expectations=(
        Expectation(
            metric="TWT agreement established + STA stays authorized",
            value="PASS with 2 STAs concurrently ('authorized STAs: 2' held for the window)",
            source="docs/worklog/2026-06-24-twt-action-frame.md:9,66",
            noisy=False,
            assertion="agreement established AND the STA remains associated. Structural.",
        ),
        Expectation(
            metric="doze RTT signature",
            value="baseline ~10-22 ms, dozing bursts to ~950-1190 ms "
                  "(.159: flat=86 elevated=9 max 1057 ms)",
            source="docs/worklog/2026-06-24-twt-action-frame.md:73-87; "
                   "docs/worklog/2026-06-23-regression-stress-test.md:22",
            noisy=True,
            assertion="reported, not asserted as an equality. If used as a proxy at all, assert "
                      "only the QUALITATIVE signature: at least one RTT >> baseline, i.e. the "
                      "STA demonstrably dozed. The distribution is timing/RF dependent.",
        ),
    ),
)

MESH_PEERING = T2Test(
    slug="mesh-peering",
    pass_if=">=1 mesh peer reaches ESTAB (SAE+AMPE), no anchor",
    title="Mesh peering (SAE + AMPE) reaches ESTAB",
    rig={"support": "board0 — test-mesh-peering (peers + beacons)",
         "reporter": "board1 — test-mesh-peering (polls ESTAB peer count)"},
    roles=(
        # Symmetric: both nodes run the SAME mesh app. The support node comes up + beacons
        # (up-marker "mesh-up"); the reporter polls mmwlan_mesh_peer_count() -> ESTAB.
        # ESP<->ESP peering needs no Linux anchor (memory: esp-esp-mesh-peers-no-anchor).
        # Light load, so board0/board1 are fine for the peering handshake.
        Role(name="support", device="board0", app="test-mesh-peering",
             boot_wait_s=30.0, up_marker="mesh-up"),
        Role(name="reporter", device="board1", app="test-mesh-peering", reporter=True),
    ),
    what_it_proves=(
        "Two ESP nodes complete SAE authentication + AMPE key exchange against each other -- "
        "with no Linux anchor -- and the plink reaches ESTAB. The foundation every mesh "
        "milestone sits on."
    ),
    what_it_does_not_prove=(
        "Forwarding, multi-hop, or on-air byte-equivalence with Linux. Peering ESTAB only "
        "proves a tolerant peer completed the handshake."
    ),
    expectations=(
        Expectation(
            metric="ESTAB mesh peer count",
            value=">= 1 (mmwlan_mesh_peer_count)",
            source="rimba-halow-mesh peer-count heartbeat; memory esp-esp-mesh-peers-no-anchor "
                   "(two ESPs peer + ping with no Linux anchor)",
            noisy=False,
            assertion="peer_count >= 1. ESTAB is binary (SAE+AMPE either closed or not) and "
                      "independent of RF quality -- a fading link changes timing, not whether "
                      "the handshake completes.",
        ),
        Expectation(
            metric="plink stability",
            value="holds (MESH_PLINK_INACTIVITY_MS 1800s = Linux MESH_DEFAULT_PLINK_TIMEOUT)",
            source="docs/mesh-ap/rimba-mesh-plink-liveness-codemap.md; "
                   "docs/worklog/2026-07-12-mesh-peering-flap-bisect-no-regression.md",
            noisy=False,
            assertion="NOT asserted by this test (it checks first-ESTAB, not a soak). The "
                      "6s->1800s liveness fix is a constant a port-forward could revert silently; "
                      "a stability soak is a separate, longer test.",
        ),
    ),
)

MESH_RELAY = T2Test(
    slug="mesh-relay",
    pass_if="origin->relay->dest delivered via the relay (SW-CCMP)",
    title="Mesh multi-hop relay (SW-CCMP forwarding)",
    rig={
        "relay": "board2 — test-mesh-relay (MANDATORY: fully wired)",
        "dest": "board1 — test-mesh-relay (responder)",
        "origin": "board0 — test-mesh-relay (pings dest via relay; reporter)",
    },
    roles=(
        # Symmetric 3-node app; role self-selected by MAC. A forced-line allowlist makes the
        # origin reach the dest ONLY via the relay. Support roles (relay, dest) boot first; the
        # origin is the reporter. The relay MUST be the fully-wired board2 -- require_wired makes
        # the orchestrator refuse board0/board1 rather than produce a false bug.
        # Each role feeds its assigned board's mesh MAC to a build var; the orchestrator passes
        # all three (origin/dest/relay) to EVERY flash so the symmetric app agrees on the line
        # topology — no MAC is hardcoded in firmware (derived from the manifest BENCH registry).
        Role(name="relay", device="board2", app="test-mesh-relay", require_wired=True,
             boot_wait_s=30.0, up_marker="mesh-up", build_mac_var="MESH_RELAY_MAC"),
        Role(name="dest", device="board1", app="test-mesh-relay",
             boot_wait_s=30.0, up_marker="mesh-up", build_mac_var="MESH_DEST_MAC"),
        Role(name="origin", device="board0", app="test-mesh-relay", reporter=True,
             build_mac_var="MESH_ORIGIN_MAC"),
    ),
    what_it_proves=(
        "A frame originated on one node is forwarded by an ESP relay to a third node with host "
        "SW-CCMP crypto intact -- the multi-hop mesh claim, end to end."
    ),
    what_it_does_not_prove=(
        "HW-crypto forwarding, which is a FIRMWARE limitation, not a bug to regress on: the "
        "MM6108 HW-crypto path emits a forwarded frame whose CCMP MIC does not verify under the "
        "relay's own group key (root-caused 2026-07-15 by offline MIC-verify). SW-CCMP "
        "(g_mesh_sw_crypto=true) is the shipping default and is what this test exercises."
    ),
    expectations=(
        Expectation(
            metric="multi-hop delivery, SW-CCMP",
            value="143/146 (~98%) multi-hop, 131/131 single-hop, ccmp_fail=0, 0 asserts",
            source="docs/worklog/2026-07-14-mesh-defrag-before-decrypt-PASS.md",
            noisy=True,
            assertion="delivery >= a WIDE floor (>=8/15) is the primary structural proof -- the "
                      "~99.6% MIC-fail regression drops frames, so it shows as ~0 replies. "
                      "datapath_rx_ccmp_failures corroborates: FAIL only if it DOMINATES (>= the "
                      "floor), since a stray 1-2 over a 2-hop line is noise, not the regression "
                      "(asserting exactly 0 would flake). Plus topology: sole peer == relay.",
        ),
    ),
    manual_steps=(
        "THE RELAY MUST BE board2. board0/board1 have WAKE + BUSY unwired "
        "(docs/reference/rimba-bench-devices.md:148-171) -- those pins are the chip's power-save "
        "/ flow-control handshake, so under sustained forwarding load the host cannot tell when "
        "the MM6108 is busy. That produced the 'relay interrupt-WDT crash', which was only ever "
        "seen on board0-as-relay and never on board2. Using board0 as the relay here does not "
        "test the code -- it tests the missing solder. This has produced false bugs twice."
    ),
)

MESH_LARGE_FRAME = T2Test(
    slug="mesh-large-frame",
    pass_if="a large (FW-fragmented) frame is forwarded origin->relay->dest with SW-CCMP intact",
    title="Large-frame mesh forwarding (FW-fragmented SW-CCMP, defrag-before-decrypt)",
    rig={
        "relay": "board2 — test-mesh-large-frame (MANDATORY: fully wired)",
        "dest": "board1 — test-mesh-large-frame (responder)",
        "origin": "board0 — test-mesh-large-frame (pings dest with a LARGE payload; reporter)",
    },
    roles=(
        # Identical forced-line rig to mesh-relay (same symmetric app family, same three build MACs);
        # the ONLY difference is a large ICMP payload that forces the FW to fragment the encrypted
        # mesh frame, exercising the defrag-before-decrypt RX path.
        Role(name="relay", device="board2", app="test-mesh-large-frame", require_wired=True,
             boot_wait_s=30.0, up_marker="mesh-up", build_mac_var="MESH_RELAY_MAC"),
        Role(name="dest", device="board1", app="test-mesh-large-frame",
             boot_wait_s=30.0, up_marker="mesh-up", build_mac_var="MESH_DEST_MAC"),
        Role(name="origin", device="board0", app="test-mesh-large-frame", reporter=True,
             build_mac_var="MESH_ORIGIN_MAC"),
    ),
    what_it_proves=(
        "A mesh frame LARGER than the 1 MHz / MCS0 single-PPDU limit is fragmented over the air by "
        "the MM6108 FW after the host computed one SW-CCMP MIC over the whole frame, and the RX "
        "reassembles the raw fragments BEFORE decrypting (encrypt->fragment) -- the "
        "defrag-before-decrypt path, end to end over a 2-hop line."
    ),
    what_it_does_not_prove=(
        "HW-crypto forwarding (a FIRMWARE limitation, same as mesh-relay), nor throughput. The "
        "small-frame forward is mesh-relay's job; this variant adds only the fragmentation/reassembly."
    ),
    expectations=(
        Expectation(
            metric="large-frame multi-hop delivery, defrag-before-decrypt",
            value="131/131 single-hop, 143/146 (~98%) multi-hop, ccmp_fail=0",
            source="docs/worklog/2026-07-14-mesh-defrag-before-decrypt-PASS.md",
            noisy=True,
            assertion="delivery >= a WIDE floor (>=8/15) over a ~1400-byte payload is the primary "
                      "proof -- a defrag/decrypt-order regression MIC-fails the reassembled frame and "
                      "drops it, so it shows as ~0 replies. datapath_rx_ccmp_failures corroborates "
                      "(FAIL only if DOMINANT). Plus topology: sole peer == relay.",
        ),
    ),
    manual_steps=(
        "THE RELAY MUST BE board2 (WAKE+BUSY wired only there) -- same constraint as mesh-relay. The "
        "large payload (PING_SIZE=1400) must exceed the air single-PPDU limit to force FW "
        "fragmentation; if a future radio/rate change stops fragmenting at 1400, bump PING_SIZE."
    ),
)

MESH_LEAF = T2Test(
    slug="mesh-leaf",
    pass_if="the relay peers 1-hop but does NOT forward (mmwlan_mesh_set_multihop(false))",
    title="Mesh leaf / single-hop mode (multihop opt-out)",
    rig={
        "relay": "board2 — test-mesh-leaf (MANDATORY: fully wired; multihop OFF)",
        "dest": "board1 — test-mesh-leaf (responder)",
        "origin": "board0 — test-mesh-leaf (pings dest; expects NO reply; reporter)",
    },
    roles=(
        # Same forced-line rig as mesh-relay; the relay calls mmwlan_mesh_set_multihop(false) so it
        # keeps its 1-hop plinks but declines to forward. The origin's assertion is INVERTED.
        Role(name="relay", device="board2", app="test-mesh-leaf", require_wired=True,
             boot_wait_s=30.0, up_marker="mesh-up", build_mac_var="MESH_RELAY_MAC"),
        Role(name="dest", device="board1", app="test-mesh-leaf",
             boot_wait_s=30.0, up_marker="mesh-up", build_mac_var="MESH_DEST_MAC"),
        Role(name="origin", device="board0", app="test-mesh-leaf", reporter=True,
             build_mac_var="MESH_ORIGIN_MAC"),
    ),
    what_it_proves=(
        "mmwlan_mesh_set_multihop(false) makes a node a LEAF: it keeps its 1-hop mesh plinks but does "
        "not forward, and does not black-hole (its peering survives). A shipped, on-air-A/B-verified "
        "runtime opt-out (P6d) that otherwise has no regression guard -- a port-forward could silently "
        "re-enable relaying, or turn the opt-out into a black hole."
    ),
    what_it_does_not_prove=(
        "The HWMP-invisibility of a leaf (its absence from other nodes' path selection) -- only that "
        "it declines to forward and keeps peering. Path-selection invisibility needs a >=3-hop "
        "topology this 3-board bench cannot form."
    ),
    expectations=(
        Expectation(
            metric="leaf blocks forwarding, keeps peering",
            value="0 replies via a leaf relay; sole peer still the relay",
            source="mmwlan_mesh_set_multihop(bool) (umac_mesh.h:190); P6d leaf/single-hop opt-out",
            noisy=False,
            assertion="DETERMINISTIC (not RF-bound): the forced allowlist makes the relay the ONLY "
                      "path to the dest, so a leaf relay declining to forward => EXACTLY 0 replies. "
                      "PASS iff sole peer == relay (peering survived, no black-hole) AND replies == 0. "
                      "ANY reply => leaf broken (relaying re-enabled); lost peering => black-hole.",
        ),
    ),
    manual_steps=(
        "THE RELAY MUST BE board2 (same wired constraint as mesh-relay). Unlike the delivery tests, "
        "this one asserts EXACTLY 0 replies (no wide floor): with the forced line there is no path to "
        "the dest except the leaf relay, so any reply is a deterministic leak, not RF noise."
    ),
)

MESH_RELAY_NOCRASH = T2Test(
    slug="mesh-relay-nocrash",
    pass_if="the relay forwards a sustained load with NO silent hw-restart (interrupt-WDT crash)",
    title="Mesh relay stability under load (no hw-restart)",
    rig={
        "origin": "board0 — test-mesh-relay-nocrash (support; drives a long ping burst)",
        "dest": "board1 — test-mesh-relay-nocrash (support; responder)",
        "relay": "board2 — test-mesh-relay-nocrash (MANDATORY fully wired; REPORTER)",
    },
    roles=(
        # Roles are RE-CAST vs mesh-relay: the RELAY is the reporter (hw_restart_counter is its own
        # stat, and only a reporter's console is scraped). So origin + dest are SUPPORT (they boot
        # first); the origin drives a long ping burst to load the relay, and the relay (flashed LAST)
        # forwards then checks it did not hw-restart. Declaration order matters -- support boots first.
        Role(name="origin", device="board0", app="test-mesh-relay-nocrash",
             boot_wait_s=30.0, up_marker="mesh-up", build_mac_var="MESH_ORIGIN_MAC"),
        Role(name="dest", device="board1", app="test-mesh-relay-nocrash",
             boot_wait_s=30.0, up_marker="mesh-up", build_mac_var="MESH_DEST_MAC"),
        Role(name="relay", device="board2", app="test-mesh-relay-nocrash", require_wired=True,
             reporter=True, build_mac_var="MESH_RELAY_MAC"),
    ),
    what_it_proves=(
        "An ESP relay forwards a sustained mesh load without a SILENT hw-restart: its "
        "hw_restart_counter does not increment across the forwarding window. This catches the "
        "root-caused interrupt-WDT SPI-host-teardown crash-and-recover that a delivery check alone "
        "can miss (a brief crash-recover inside a ping window still delivers most pings)."
    ),
    what_it_does_not_prove=(
        "Throughput, nor the SW-CCMP correctness that mesh-relay covers -- only that the relay stays "
        "up (no hw-restart) while forwarding. It needs REAL forwarding load: if the endpoints never "
        "peer the relay, it lands INCONCLUSIVE (a relay under no load cannot crash under load)."
    ),
    expectations=(
        Expectation(
            metric="relay hw_restart_counter across a forwarding window",
            value="hw_restart_counter unchanged (0 restarts) over ~40s of forwarding",
            source="hw_restart_counter (mmwlan_stats.h:65); interrupt-WDT SPI-host-teardown crash "
                   "root-caused 2026-07-12",
            noisy=False,
            assertion="PASS iff BOTH endpoints peered the relay (real forwarding load) AND "
                      "hw_restart_counter did not change across the window. FAIL if it climbed (a "
                      "silent hw-restart under load). INCONCLUSIVE if the relay never got both peers "
                      "(no load -> a crash-under-load cannot be judged; never a false PASS).",
        ),
    ),
    manual_steps=(
        "THE RELAY MUST BE board2 -- doubly so here: the interrupt-WDT crash this guards was only ever "
        "seen on a WIRED relay (board2); board0/board1 as relay produce a DIFFERENT (missing-solder) "
        "crash, so using them tests the wrong thing. Timing is sized to REPORTER_TIMEOUT_S (130s): "
        "relay boot + peer-wait (45s) + window (40s); widen the window budget if a cold bench needs "
        "longer to peer both endpoints."
    ),
)

MESH_AP = T2Test(
    slug="mesh-ap",
    pass_if="both vifs beacon + STA routes through, ttl=63 (1 hop)",
    title="Mesh + AP concurrency on one MM6108 (the mesh-gate)",
    rig={"gate": "board2 — test-mesh-ap-gate (mesh vif + AP vif + ip_forward; MANDATORY wired)",
         "mesh_peer": "board1 — test-mesh-ap-peer (far node 10.9.9.100, return route via gate)",
         "sta": "board0 — test-mesh-ap-sta (behind the AP; pings the far node; reporter)"},
    roles=(
        # gate (board2): mesh + AP + ip_forward on one radio. MANDATORY board2 (runs concurrency +
        # forwarding under load). Boots first, longest (up_marker "gate-ready").
        Role(name="gate", device="board2", app="test-mesh-ap-gate", require_wired=True,
             boot_wait_s=35.0, up_marker="gate-ready"),
        # mesh_peer (board1): the far mesh node the STA pings; its return route to the AP subnet
        # goes via the gate's mesh IP (10.9.9.108).
        Role(name="mesh_peer", device="board1", app="test-mesh-ap-peer",
             boot_wait_s=30.0, up_marker="mesh-peer-up"),
        # sta (board0): associates to the gate's AP, pings the far node, asserts ttl=63. Reporter.
        Role(name="sta", device="board0", app="test-mesh-ap-sta", reporter=True),
    ),
    what_it_proves=(
        "Both vifs beacon concurrently on a single radio and a STA associated to the AP half "
        "routes through the mesh half (the all-ESP mesh-gate, hw-proven 2026-07-08/07-09)."
    ),
    what_it_does_not_prove=(
        "Gate throughput -- the recorded stress matrix found the gate IS the throughput "
        "bottleneck (cross-gate 30-62% loss vs mesh-to-mesh 3-13% under flood). That is a known "
        "characteristic, not a regression, so it must never be asserted as a pass/fail."
    ),
    expectations=(
        Expectation(
            metric="both vifs beacon concurrently + STA forwards through",
            value="STA->gate->mesh->far-node end-to-end 10/10, ttl=63",
            source="docs/worklog/2026-07-09-esp32-mesh-ap-end-to-end-forward.md",
            noisy=False,
            assertion="both vifs up AND >=1 end-to-end delivery. Structural. ttl=63 is a good "
                      "assert: it PROVES the packet was IP-forwarded exactly one hop, which a "
                      "delivery count alone does not.",
        ),
    ),
)

MESH_LINUX = T2Test(
    slug="mesh-linux",
    pass_if="ESTAB peer == the Linux node's MAC + >=1 ICMP reply",
    title="ESP <-> Linux 802.11s mesh interoperability",
    rig={"linux": "chronite — a real Linux mesh node (mac80211 + morse_driver), brought up over ssh",
         "esp": "board2 — test-mesh-linux (reporter): peers with + pings the Linux node"},
    roles=(
        # Linux support role: the orchestrator brings chronite up as a `rimba-smesh` mesh point
        # over ssh (tools/regtest/linux_peer.py), and tears it back down afterwards. No app / no
        # up-marker (it's a Linux node, not an ESP the orchestrator flashes).
        Role(name="linux", device="linux:chronite", linux_setup="mesh-peer"),
        # ESP reporter: joins the same mesh, asserts an ESTAB peer == the Linux node's MAC, pings it.
        Role(name="esp", device="board2", app="test-mesh-linux", reporter=True),
    ),
    what_it_proves=(
        "The ESP completes SAE+AMPE mesh peering against a REAL Linux stack (not another ESP running "
        "the same morselib) AND exchanges ICMP data with it -- the gold-standard interop check. "
        "Verified 2026-07-16: board2 peered with chronite + 13/15 replies."
    ),
    what_it_does_not_prove=(
        "Multi-hop relay through a Linux node, or on-air frame byte-equivalence (that is the "
        "separate chronium-monitor capture the on-air rule points at)."
    ),
    expectations=(
        Expectation(
            metric="ESTAB peer == the Linux node MAC + ICMP replies",
            value="peered (SAE+AMPE) with chronite (3c:22:7f:37:51:38) + >=6/15 replies from 10.9.9.2",
            source="memory mesh-status-p4-esp-linux-ping (ESP joins a Linux HaLow mesh, pings); "
                   "docs/reference/captures/wpa-smesh.conf (rimba-smesh / SAE rimbamesh2026 / ch27)",
            noisy=True,
            assertion="peering (an ESTAB peer whose MAC == the Linux node's) is the binary "
                      "structural half -- SAE+AMPE either closed against mac80211 or it didn't; the "
                      "reply count is RF-bound (wide floor + INCONCLUSIVE band, 0-despite-peering=FAIL).",
        ),
    ),
    manual_steps=(
        "The Linux side is automated: linux_peer.bring_up_mesh(chronite) pushes the reference "
        "wpa-smesh.conf if missing, starts wpa_supplicant_s1g in mesh mode, and tears it down after. "
        "chronite must be reachable by ssh (it is, via ~/.ssh/config). The ESP mesh ID rimba-smesh "
        "MUST match the Linux config (it does)."
    ),
)

TWT_ASSOC = T2Test(
    slug="twt-assoc",
    pass_if="assoc-embedded TWT INSTALLED (flow 0..3) on a Linux AP",
    title="Assoc-embedded TWT — INSTALLED on a Linux AP (the universal path)",
    rig={"linux": "chronite — Linux hostapd_s1g SoftAP (rimba-ping/SAE/dtim1), brought up over ssh",
         "sta": "board1 — test-twt-assoc-sta (requester + reporter)"},
    roles=(
        # Linux AP support role: the orchestrator brings chronite up as a hostapd_s1g SoftAP over
        # ssh (linux_peer.bring_up_ap) and tears it down afterwards. The DISCRIMINATOR: a Linux AP
        # ignores the mid-session TWT action (the `twt` test), but honours the assoc-embedded IE.
        Role(name="linux", device="linux:chronite", linux_setup="hostapd-ap"),
        # STA reporter: TWT via mmwlan_twt_add_configuration BEFORE connect (in the assoc IEs),
        # then polls the agreement -> INSTALLED (scans flows 0..3).
        Role(name="sta", device="board1", app="test-twt-assoc-sta", reporter=True),
    ),
    what_it_proves=(
        "An assoc-embedded TWT agreement (the TWT IE in the association request) reaches INSTALLED "
        "against a REAL Linux hostapd_s1g AP -- the UNIVERSAL TWT path that engages on BOTH APs, "
        "unlike the mid-session action the `twt` test uses (a Linux AP ignores that). Verified "
        "2026-07-17: INSTALLED on flow 0 against both the ESP SoftAP and chronite's hostapd."
    ),
    what_it_does_not_prove=(
        "The power saving itself (that is the tp PPK2 tier, not a pass/fail). The mmwlan_twt_"
        "agreement_installed accessor turned out to cover the assoc-embedded path too (the earlier "
        "'no accessor for the assoc path' caveat was refuted on-bench)."
    ),
    expectations=(
        Expectation(
            metric="assoc-embedded TWT agreement state",
            value="INSTALLED on flow 0 (mmwlan_twt_agreement_installed == 1), on ESP AP + Linux AP",
            source="on-bench 2026-07-17 (docs/worklog/2026-07-17-powersave-test-cases-batch.md); "
                   "accessor umac.c:1749; add_configuration path umac_twt.c:339",
            noisy=False,
            assertion="INSTALLED is a discrete negotiation outcome (EMPTY->PENDING->INSTALLED), "
                      "RF-independent. PASS iff an agreement reaches INSTALLED on any of flows 0..3; "
                      "a staged-but-not-installed agreement is INCONCLUSIVE, not FAIL.",
        ),
    ),
)


MULTI_TWT = T2Test(
    slug="multi-twt",
    pass_if="both STAs reach TWT INSTALLED concurrently",
    title="Multi-STA TWT — two STAs both reach INSTALLED concurrently",
    rig={"ap": "board0 — test-apsta-ap (SoftAP, TWT responder default-on)",
         "sta1": "board1 — test-twt-sta (reporter)",
         "sta2": "board2 — test-twt-sta (reporter)"},
    roles=(
        Role(name="ap", device="board0", app="test-apsta-ap", boot_wait_s=25.0,
             up_marker="ap-ready"),
        # TWO reporters (the harness flashes+captures each; the first stays associated while the
        # second joins, so both coexist on the AP). Verdict = both INSTALLED. Reuses the mid-session
        # test-twt-sta -- no new firmware. board2 must be powered (tools/ppk2_hold.py).
        Role(name="sta1", device="board1", app="test-twt-sta", reporter=True),
        Role(name="sta2", device="board2", app="test-twt-sta", reporter=True),
    ),
    what_it_proves=(
        "Two STAs concurrently negotiate a TWT agreement against one ESP SoftAP -- each reaches "
        "INSTALLED while the other stays associated (the '2 authorized STAs' concurrency the single-"
        "STA `twt` test asserts but never actually runs). Exercises the per-STA responder agreement "
        "table (umac_twt responder_peers[], PSRAM-routed)."
    ),
    what_it_does_not_prove=(
        "The power saving itself, nor a high STA count (2 boards is the bench ceiling for STA "
        "reporters; the AID>=64 / many-STA admission path is unexercised)."
    ),
    expectations=(
        Expectation(
            metric="both STAs reach TWT INSTALLED concurrently",
            value="sta1 INSTALLED AND sta2 INSTALLED (both associated at the end)",
            source="multi-reporter harness (t2_onair _record_multi_verdict); per-STA responder "
                   "table docs/worklog/2026-06-23-ap-sta-ceiling-100-psram.md §6",
            noisy=False,
            assertion="PASS iff EVERY reporter's TEST|RESULT is PASS (each reached INSTALLED). "
                      "Any reporter FAIL/no-RESULT -> FAIL; any INCONCLUSIVE (no FAIL) -> "
                      "INCONCLUSIVE. Structural, RF-independent.",
        ),
    ),
)


MESH_AP_MULTI_TWT = T2Test(
    slug="mesh-ap-multi-twt",
    pass_if="both STAs behind the gate reach TWT INSTALLED",
    title="Mesh-gate serving multiple TWT STAs (mesh+AP concurrency + per-STA TWT)",
    rig={"gate": "board2 — test-mesh-ap-gate (mesh vif + AP vif on one radio; MANDATORY wired)",
         "sta1": "board0 — test-twt-sta (behind the gate's AP; TWT reporter)",
         "sta2": "board1 — test-twt-sta (behind the gate's AP; TWT reporter)"},
    roles=(
        # Gate = board2: 802.11s mesh vif + co-channel SoftAP (SSID rimba-ping) + max_stas=4 on ONE
        # MM6108. Boots first, longest (up_marker "gate-ready"). MANDATORY wired.
        Role(name="gate", device="board2", app="test-mesh-ap-gate", require_wired=True,
             boot_wait_s=35.0, up_marker="gate-ready"),
        # Two TWT STAs behind the gate's AP half. Each REUSES test-twt-sta (the gate's AP is the
        # same SSID rimba-ping / SAE). Both reporter=True -> the multi-reporter harness ANDs the
        # verdicts. An AP vif is a TWT responder by default (umac_interface.c:145), so the gate's
        # SECONDARY AP vif should answer -- which this test is the first to actually exercise.
        Role(name="sta1", device="board0", app="test-twt-sta", reporter=True),
        Role(name="sta2", device="board1", app="test-twt-sta", reporter=True),
    ),
    what_it_proves=(
        "The all-ESP mesh-gate (mesh vif + AP vif on one MM6108) serves MULTIPLE concurrent TWT "
        "agreements on its AP half -- two STAs behind the gate each reach TWT INSTALLED while the "
        "mesh vif is up. Exercises the gate's per-STA TWT responder table (responder_peers[], PSRAM) "
        "under mesh+AP concurrency -- a combination no other test (mesh-ap: 1 STA no TWT; multi-twt: "
        "TWT but a plain AP) covers."
    ),
    what_it_does_not_prove=(
        "TWT + mesh ROUTING at the same time (only 3 boards: gate + 2 STAs, so the gate's mesh half "
        "has no far peer here), nor the power saving itself. High STA-count TWT (>2) is unexercised."
    ),
    expectations=(
        Expectation(
            metric="both STAs reach TWT INSTALLED behind the gate",
            value="sta1 INSTALLED AND sta2 INSTALLED, both associated to the gate's AP",
            source="mesh-gate hw-proven docs/mesh-ap/rimba-mesh-ap-milestones.md; per-STA TWT "
                   "responder table docs/worklog/2026-06-23-ap-sta-ceiling-100-psram.md §6; the "
                   "combination is new 2026-07-17",
            noisy=False,
            assertion="PASS iff EVERY reporter reached TWT INSTALLED (structural, RF-independent). If "
                      "the gate's secondary AP vif does not answer TWT, a reporter lands INCONCLUSIVE "
                      "-> the combined verdict is INCONCLUSIVE, not FAIL (an honest untested-path "
                      "result, the first time this path runs).",
        ),
    ),
)


T2_TESTS: tuple[T2Test, ...] = (
    SWCCMP,
    AMPDU_CAP,
    AP_STA_PING,
    IBSS,
    TWT,
    TWT_ASSOC,
    MULTI_TWT,
    MESH_PEERING,
    MESH_LINUX,
    MESH_RELAY,
    MESH_LARGE_FRAME,
    MESH_LEAF,
    MESH_RELAY_NOCRASH,
    MESH_AP,
    MESH_AP_MULTI_TWT,
)

T2_BY_SLUG = {t.slug: t for t in T2_TESTS}
