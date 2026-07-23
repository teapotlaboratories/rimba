# 2026-07-22 — Mesh-gate port S3 (6-address AE_A5_A6 datapath): VERIFIED ON-AIR + a major de-risk

**Status:** S3 datapath primitive WRITTEN + COMPILES + **ON-AIR VERIFIED (PASS, ESP↔ESP)** — an ESP builds a
6-address Address-Extension mesh data frame, the MM6108 FW transmits + delivers it, and the receiving ESP
decrypts + parses the AE Mesh Control and extracts the proxied endpoints byte-exactly. **Uncommitted** in the
`components/halow` submodule; new `firmware/test-mesh-ae` + probe additions to `firmware/test-mesh-gate-rx`
untracked. *(HTML render: TODO next session.)*

## Goal

Third stage of the approved 802.11s **Mesh-gate** port (`docs/mesh-ap/rimba-mesh-ap-mesh-gate-discovery-design.md`),
after S1 (gate RANN TX) + S2 (RANN RX). S3 is the **6-address Address-Extension (AE_A5_A6) datapath primitive**:
the ability to emit + parse a *proxied* mesh data frame whose Mesh Control carries two extra addresses — eaddr1
(addr5 = the final DA) + eaddr2 (addr6 = the original off-mesh SA) — the mechanism a gate uses to carry an
off-mesh host's traffic through the mesh. Port of `net/mac80211` `ieee80211_new_mesh_header` (TX) +
`ieee80211_rx_mesh_data` (RX), kernel 6.12.21.

## 🔑 The major de-risk: CCMP is AE-transparent (the design's "hard 20%" collapses)

The design flagged S3 as the "hard 20%" with a headline risk: *"CCMP offset/AAD math (~528–628) must be
re-derived for the +12 AE bytes or proxied frames MIC-fail on relay."* **This does NOT apply to our architecture.**
The mesh datapath already uses **host SW-CCMP** (built for #20, because the MM6108 HW-crypto path has an
A4-sensitive delivery gate). In SW-CCMP:
- The **Mesh Control (incl. the AE eaddrs) is part of the CCMP-encrypted BODY**, not the 802.11 MAC header.
- The **CCMP AAD is over the MAC header only** — `mesh_ccmp_aad_nonce` (ccmp.c) reads masked FC + A1/A2/A3 +
  masked Seq + A4 (present, since mesh unicast is already 4-addr) + masked QoS. It never touches the Mesh Control
  or body. The nonce reads only A2 + the PN.
- `body_len` is computed dynamically (`total − hdr_total`), so the +12 AE bytes are absorbed automatically.

So adding the 12 AE bytes only lengthens the encrypted plaintext; the AAD/nonce/MIC are byte-identical to the
non-AE case. **Zero CCMP change was needed** (ccmp.c is untouched). A Linux peer computes the same MIC. This was
confirmed independently by the adversarial verification workflow (below).

## What was implemented (components/halow, uncommitted)

`umac_mesh.c` (+~180 for S3; combined S1+S2+S3 = 358 ins) + `umac_datapath.c` (+13) + `umac_mesh.h` (+~20):
- **TX AE build** — `struct mesh_forward_params` += `ae` + `eaddr1[6]` + `eaddr2[6]`; `umac_mesh_build_forward`
  emits an **18-byte** `AE_A5_A6` Mesh Control when `ae` (flags=0x02, ttl, seqnum le32, eaddr1, eaddr2) — the exact
  `struct ieee80211s_hdr` layout — after the QoS(0x0100) + 4-addr header. Non-AE stays the 6-byte flags=0x00 control
  (the existing relay path, byte-unchanged).
- **TX injector** — `mmwlan_mesh_send_ae_test(mesh_da, eaddr1_da, eaddr2_sa, payload, len)`: originates a proxied AE
  frame to a directly-peered node, reusing the proven forward TX path (next-hop peer stad, SW-CCMP,
  `umac_datapath_tx_mesh_unicast_frame`). A manual injector for verifying S3 (NOT the gate proxy datapath — S4/S5).
- **RX eaddr extract** — the RX Mesh-Control block (`umac_datapath.c`) already computed the AE length (6/12/18); S3
  makes it **extract** eaddr1=`mctrl[6..11]`/eaddr2=`mctrl[12..17]` (bounds-checked) instead of discarding them, and
  records them via `umac_mesh_note_ae_rx`. S4 will replace the record with `mpp_path_add(eaddr2, mesh_sa)` + proxied
  delivery to eaddr1.
- **Probe accessor** — `mmwlan_mesh_ae_rx_probe(eaddr1_out, eaddr2_out)` returns the count of received AE frames +
  the latest eaddrs, so a fixture can confirm the FW delivered + the host parsed the AE frame (morselib MMLOG is not
  on the ESP UART).

**Fixtures:** `firmware/test-mesh-ae` (AE sender; targets a mesh node via `LINUX_MAC=`, default chronite; peers +
injects every 3s) + a probe poll added to `firmware/test-mesh-gate-rx` (receiver: logs `AE rx count` + the eaddrs).

## ✅ VERIFICATION — on-air, ESP↔ESP round-trip (PASS)

Because mesh data is **encrypted**, a passive monitor (chronium) cannot byte-diff the plaintext AE Mesh Control —
so the verification is **receiver-side decode**: board0 sends AE frames with known eaddrs; board1 extracts them.

Bench: **board0** (ttyACM0, mesh MAC `e2:72:a1:f8:ef:a4`) = `test-mesh-ae` targeting board1
(`LINUX_MAC=e2:72:a1:f8:f9:40`); **board1** (ttyACM1, mesh MAC `e2:72:a1:f8:f9:40`) = `test-mesh-gate-rx` receiver.
Both on `rimba-smesh` (SAE), TX power 1 dBm. board0 injects `eaddr1_da=02:00:00:00:00:bb`,
`eaddr2_sa=02:00:00:00:00:aa` every 3 s.

board1 serial:
```
AE rx count=24  last eaddr1(DA)=02:00:00:00:00:bb  eaddr2(SA)=02:00:00:00:00:aa
```
The count climbs steadily (board1 receives ~every 3 s) and the extracted eaddrs are **byte-exact** to what board0
sent. This proves the whole AE datapath primitive:
1. board0 built the `AE_A5_A6` frame (18-byte Mesh Control, flags 0x02, eaddr1/eaddr2). 2. SW-CCMP encrypted it (AE
in the body). 3. **the FW accepted + transmitted the AE frame.** 4. **the FW on board1 DELIVERED it to the host** —
this **resolves the design's #1 open risk** ("does the MM6108 FW deliver/accept AE frames"). 5. board1 SW-CCMP
decrypted it (MIC OK — AE-transparent CCMP). 6. board1 parsed the AE Mesh Control + extracted eaddr1/eaddr2 exactly.

**Independent static verification:** a 3-lens adversarial workflow (ae-bytes-semantics, ccmp-transparent-regression,
rx-safety-flow) + synthesis returned **MATCH on every lens, zero blockers** — confirming the 18-byte AE Mesh Control
== `struct ieee80211s_hdr`, the eaddr semantics (eaddr1=addr5=proxied DA, eaddr2=addr6=proxied SA) match Linux
`ieee80211_rx_mesh_data`/`ieee80211_new_mesh_header`, the RX offsets + bounds-check are safe, the non-AE relay path
is byte-unchanged, and — proven, not assumed — **CCMP is AE-transparent** (AAD reads only the MAC header, `body_len`
absorbs +12, frame is 4-addr so A4 is in the AAD regardless).

**Radio-silent cleanup done** ([[radio-silent-after-every-test]]): board0 + board1 → `rimba-hello`; chronite +
chronium `wlan1` down (chronium `type managed`).

## Deferred / not done
- **Linux-interop probe (bonus, not required).** The stronger gold-standard check — ESP → a Linux mesh node, then
  `iw dev wlan1 mpp dump` shows the proxied eaddr2 via the ESP (Linux's `mpp_path_add` validates the AE bytes) — was
  **blocked by chronite's flaky mesh** this session (it came up `type mesh point` with **no channel** repeatedly; a
  morse-driver reload hung). The ESP↔ESP round-trip already proves the primitive + resolves the FW-delivery risk; the
  Linux-interop A/B is a nice-to-have to add when chronite's mesh is healthy (or use chronogen). `test-mesh-ae`
  already supports it: `make flash APP=test-mesh-ae LINUX_MAC=<linux wlan1 MAC>`.
- **Multicast AE_A4** (mcast/ARP proxy, flags 0x1, proxied_addr=eaddr1) — out of S3 (A5_A6) scope; future gate-proxy
  coverage (the workflow flagged it as `na`).

## Next step — S4 (MPP proxy table + learning + send_to_gates)
S3 extracts eaddr1/eaddr2 but only records them. S4 adds a real **MPP table**: on AE RX, `mpp_path_add(eaddr2,
mesh_sa)` (learn the off-mesh host is reachable via that mesh node) + proxied delivery to eaddr1; on TX-to-an-unknown
dest, a `send_to_gates` fallback that walks the S2 `known_gates` and emits an AE frame via the gate. Design §S4;
watch the FW A4≠TA withhold ([[gateway-e2e-forward-mesh-unicast-gap]]) for 6-addr forwards. ~3-4 d.

## Footguns
- **CCMP is AE-transparent here** — do NOT "re-derive the CCMP for AE"; the design's warning predates the SW-CCMP
  architecture. The AE eaddrs are encrypted body; the AAD is MAC-header-only; the frame is already 4-addr.
- **morselib MMLOG is not on the ESP UART** — to observe a datapath event from a fixture, add an `mmwlan_*` probe
  accessor + poll it via `ESP_LOGI` (that's what `mmwlan_mesh_ae_rx_probe` is for). This bit both S1/S2 and S3.
- **`LINUX_MAC=` needs CMake propagation** — a fixture's `main/CMakeLists.txt` must
  `target_compile_definitions(${COMPONENT_LIB} PRIVATE "TEST_LINUX_MAC=\"${TEST_LINUX_MAC}\"")` or the `-D` is
  ignored and the `#ifndef` default is used (this silently made board0 target chronite, not board1, at first).
- **chronium radio silence:** `pkill -9 -f wpa_supplicant_s1g`, NOT bare `wpa_supplicant` (kills its LAN supplicant).
- **chronite mesh is flaky** — `type mesh point` with no channel = the FW mesh BSS didn't start; a Pi 5 driver
  reload sometimes hangs. Re-scp `wpa-smesh.conf`, retry, or use another Linux node.
- Re-`scp` the Linux ref (`net/mac80211/{mesh,mesh_hwmp,mesh_pathtbl,rx,tx}.c` + `include/linux/ieee80211.h`) from
  `chronium:/home/chronium/halow/rpi-linux/` each session (kernel 6.12.21).

## Files
- `components/halow/.../umac/mesh/umac_mesh.c` (+358 S1+S2+S3), `.../umac_mesh.h` (+~53),
  `.../umac/datapath/umac_datapath.c` (+13) — **uncommitted**.
- `firmware/test-mesh-ae/` — AE sender/injector fixture (untracked). `firmware/test-mesh-gate-rx/` — receiver
  (AE probe poll added).
- `docs/mesh-ap/rimba-mesh-ap-mesh-gate-discovery-design.md` — design (S1/S2/S3 marked done; §3 table re-pin pending).
- Memory: `mesh-gate-8021s-port-planned` (updated: S3 verified).
