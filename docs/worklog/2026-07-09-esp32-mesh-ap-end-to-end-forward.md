# 2026-07-09 — all-ESP Mesh-gate end-to-end: forward works, reply return-leg doesn't (crypto/ARP/TX-build all exonerated)

The headline: on the all-ESP Mesh-gate (mesh + SoftAP co-channel on one MM6108), an ESP STA under
the gateway's AP still **cannot** complete a ping to a 2nd mesh node — **but** the failure was
narrowed all the way from a vague "forward doesn't work" down to a specific **reply return-leg**
failure. On-air captures + on-node counters this session **exonerated** the gateway datapath TX
build, lwIP `ip_forward`, ARP resolution, and the *entire* CCMP crypto/decrypt path. board1
receives and decrypts the forwarded ICMP perfectly; its echo-reply just never gets back to the
gateway. Separately, the `umac_keys.c` AP-downlink fix was confirmed **non-regressive** to the mesh.

Branch: `components/halow` → `feat/mesh-ap-concurrency` (UNCOMMITTED). This is the Stage-3 on-air /
Gap-E end-to-end piece deferred by `docs/mesh-ap/rimba-mesh-ap-esp32-stage1-codemap.md` and
`docs/mesh-ap/rimba-mesh-ap-esp32-stage4-datapath-design.md`. Self-contained: every issue, the
bench setup, what was tried, and each result are recorded here so it can resume cold.

**On-air verification status (per the AGENTS.md two-tier bar):** the gateway→board1 forwarded
unicast was captured on chronium's `morse0` monitor and decodes as a correct 4-address CCMP mesh
frame (matches source layout). The **gold-standard live-Linux A/B is BLOCKED** — the only Linux
mesh node (chronite) can't peer with the SAE ESP mesh (open-vs-SAE mismatch, Phase 2), and the
frame in question is one only a *forwarding gateway* emits, so there is no live Linux gateway on
this bench to diff against. Named blocker, not a skipped step.

---

## Bench setup (all-ESP topology — the reproducible rig)

```
 board2 (STA)               board0 (GATEWAY)                     board1 (2nd mesh node)
 rimba-halow-sta-perf       rimba-halow-mesh-ap                  rimba-halow-mesh
 /dev/ttyACM4 (PPK2)        /dev/ttyACM0                         /dev/ttyACM1
 192.168.12.<mac[5]>  --AP--> AP  192.168.12.1  (SSID rimba-ping, SAE)
 (~.51, STA mac                  BSSID e0:72:a1:f8:ef:a4
  bc:2a:33:96:b2:33)            ip_forward (CONFIG_LWIP_IP_FORWARD=y)
                                MESH 10.9.9.136  --mesh(ch27)-->  10.9.9.100
                                mesh mac e2:72:a1:f8:ef:a4        mesh mac e2:72:a1:f8:f9:40
```

- **Gateway = board0** (`rimba-halow-mesh-ap`): MESH on the primary vif + SoftAP on the secondary
  vif, a 2nd `esp_netif` for the AP subnet, per-vif RX ext-cbs, lwIP `ip_forward` between
  `10.9.9.0/24` and `192.168.12.0/24`. TX capped to **1 dBm** (`mmwlan_override_max_tx_power(1)`).
  Mesh id `rimba-mesh`, ch27.
- **2nd node = board1** (`rimba-halow-mesh`): plain 802.11s mesh, static `10.9.9.100`, responder.
  Default gw `10.9.9.1`; for the end-to-end return route it must be `10.9.9.136` (a 1-line test
  edit, reverted after runs).
- **STA = board2** (`rimba-halow-sta-perf`, `#define IPERF 1`): associates to `rimba-ping`, static
  `192.168.12.<mac[5]>`, gw `.1`, esp_console REPL to drive `ping <ip>` over serial. **PPK2-gated**:
  only enumerates as `/dev/ttyACM4` while `tools/ppk2_hold.py` runs.
- **Sniffer = chronium** (`ssh chronium`): `morse0` S1G monitor on ch27, `~/halow-mon.py`.
- **chronite** (Linux mesh node): a DEAD-END for this test — see Phase 2.

### The mesh is SAE-secured, not open (important)
`rimba-halow-mesh-ap`'s `mmwlan_mesh_args` sets no passphrase, which *looks* open, but mesh security
on the ESP is a **compile-time** choice: with `MMWLAN_MESH_SEC_PHASE1` defined
(`umac_mesh.c:606-616`) every peer stad gets `MMWLAN_SAE` + a derived per-pair MTK at ESTAB. So the
ESP mesh runs **SAE + AMPE + CCMP**, and the `mesh_args` passphrase field is irrelevant to mesh
security. This is why chronite (open `key_mgmt=NONE`) can't peer with the ESPs.

### Reproducible commands
- **pyserial only in the IDF venv:** `PY=/home/quartz/.espressif/python_env/idf5.4_py3.13_env/bin/python`
  (the system `python3` has no `serial` module).
- **Flash:** `make flash APP=<app> BOARD=proto1-fgh100m PORT=/dev/ttyACMx` (after
  `source vendor/esp-idf/export.sh`).
- **Power board2:** `nohup $PY tools/ppk2_hold.py >/tmp/ppk2_hold.log 2>&1 &`; off with
  `$PY tools/ppk2_hold.py off` (kill the held instance first). board2 then appears as `ttyACM4`.
- **Sniffer:** `ssh chronium 'sudo ip link set morse0 up'` then
  `ssh chronium 'sudo python3 ~/halow-mon.py <secs> [ta_prefix_hex]'`. Radio already tuned to
  **915500 kHz = S1G ch27** (`morse_cli -i wlan1 channel`); `iw dev morse0 set freq` is rejected
  (-22), don't bother.

---

## Phase 1 — Mesh regression: does the `umac_keys.c` AP-downlink fix break the mesh? → NO

**Context.** The prior fix (tasks #15/#16) scoped the mesh SW-CCMP FW-offload skip in
`umac_keys_mmdrv_install_key` from the global `umac_mesh_is_active()` to a vif-type check
(`key_on_mesh_vif`), so the concurrent AP client's key gets FW-installed while mesh keys stay
host-side. That touches the shared key-install path → needs a mesh regression check.

**Setup.** Flashed board0+board1 with `rimba-halow-mesh-perf` (console build), confirmed peering,
drove `ping 10.9.9.100` from board0's console.

**Result: PASS — board0↔board1 ping 5/5, 0% loss** over the SAE mesh with host SW-CCMP. The fix is
behaviourally identical for a pure mesh (mesh keys on the mesh vif → still SW-CCMP-skipped), and
empirically the mesh is intact.

**Issues hit:** base `python3` lacks `serial` (use the IDF venv python); `rimba-halow-mesh-perf`'s
heartbeat prints `mesh alive, uptime=...` *without* `estab_peers` (confirm peering via console
ping); reset ESPs via DTR/RTS pulse to catch the boot static-IP line (prints once, early).

---

## Phase 2 — End-to-end topology selection; chronite mesh dead-end

- **`rimba-halow-sta` is netif-free / zero-traffic** (a triggered power-save ladder) — associates
  at L2 but no IP netif, can't ping. The right STA is **`rimba-halow-sta-perf`**.
- **chronite cannot be the 2nd mesh node.** Its `/home/chronite/wpa-mesh.conf` is `mode=5,
  key_mgmt=NONE` (**open**); the ESPs run **SAE** mesh. chronite *hears* the ESP beacons fine
  (`iw scan`: rimba-ping -40 dBm, "Unknown S1G Compressed" mesh beacon -39 dBm, both freq 5560=ch27)
  but forms **zero plinks** — a security-profile mismatch, not RF. **Use board1 (ESP) as the 2nd
  node.** (Supersedes the earlier belief that "chronite's open mesh was never validated"; it's a
  genuine open-vs-SAE mismatch.)
- **Gateway peers fine as a RESPONDER:** board0-gateway ↔ board1-plain-mesh gives `mesh_peers=1` /
  `estab_peers=1`. The concurrent AP does not break mesh peering. (The gateway app lacks the
  beacon→`peer_open` *initiator* wiring the standalone app has, but S1G peering is driven in
  morselib's datapath, so it peers as a responder anyway.)

Final rig = all-ESP: board0 gateway, board1 mesh node, board2 STA (PPK2-powered) — the bench's
designed roles (`docs/reference/rimba-bench-devices.md`).

---

## Phase 3 — The forward symptom + host-path exoneration

**Symptom.** board2 `ping 10.9.9.100` → 100% loss. Gateway counters during the ping: `ap_rx` climbs
(10→19), `ap_tx`+`mesh_rx` stay flat → the gateway receives board2's ICMP on the AP vif but forwards
nothing to the mesh.

**Config/code verified sound:** `CONFIG_LWIP_IP_FORWARD=y` in the built sdkconfig; RX path
(`gw_ap_rx_cb → esp_netif_receive(ap_netif)`) hands the packet to lwIP; the gateway TX datapath was
read end-to-end and is correct — `umac_datapath_gateway_lookup`→`umac_mesh_get_peer_stad(board1)`
returns board1's peer stad, `umac_datapath_gw_dequeue_tx_frame` drains mesh peer queues
(`mesh_get_next_tx_stad` scans common+peers), peer stads get `mesh_ctx.vif_id` (`umac_mesh.c:602`)
stamped at `umac_datapath.c:2110`, `construct_80211_data_header_mesh` builds a correct 4-addr, and
the SW-CCMP encrypt path is right.

**Instrument (temporary, reverted): mesh-netif TX trace** — `ESP_LOGW("MESHTX len=… dst=…
etype=…")` in `mmhalow.c:halow_transmit`. During a ping the gateway logged:
```
MESHTX len=42  dst=ff:ff:ff:ff:ff:ff  etype=0806   ← ARP request for board1 (lwIP IS forwarding)
MESHTX len=106 dst=e2:72:a1:f8:f9:40  etype=0800   ← the ICMP, to board1's REAL mesh MAC
```
→ **lwIP `ip_forward` works**: it ARPs board1, resolves it, sends the ICMP to board1's correct MAC;
`mmwlan_tx_pkt` returns SUCCESS.

**vif-tag note.** `halow_transmit` builds `metadata = { .tid = 0 }` — never sets `.vif`, so mesh TX
egresses `MMWLAN_VIF_UNSPECIFIED (0)`, not `MMWLAN_VIF_STA (1)`. Tagging `VIF_STA` (the datapath.h
design-correct value) was tried — the gateway lookup falls back to the same mesh path for
UNSPECIFIED-unicast anyway, and it **did not fix** the ping. Reverted; noted as design-correct but
not the bug.

---

## Phase 4 — On-air capture: the gateway TX is CORRECT; blocker reframed to mesh ARP

Brought up `morse0` (already on 915500 kHz=ch27) and captured during a ping. The gateway→board1
unicast **is on-air, well-formed, correctly encrypted**:
```
DATA-4addr  TA=e2:72:a1:f8:ef:a4(gw mesh)  A1/RA=e2:72:a1:f8:f9:40(board1)
            A3/DA=board1  A4/SA=gw  | after-hdr: 00 01 | 03 00 00 20 00 00 00 00 | …
            → QoS mesh-present (00 01) + CCMP header (KeyID 0, PN=3)
```
This **exonerates the gateway TX datapath** — it radiates exactly a correct 4-address CCMP mesh
unicast.

**Reframe #1 — the real blocker is mesh ARP resolution, and it's INTERMITTENT.** Consistent on-air
pattern: board2→AP uplink lands every time
(`DATA-3addr toDS=1 TA=bc:2a:33:96:b2:33 A1=e0:72:a1:f8:ef:a4`=AP BSSID); the gateway sends only
**one** ARP-for-board1 broadcast then gives up (lwIP single-ARP-then-drop-queued-packet); board1
**group-re-broadcasts** that ARP but usually does **not** unicast-reply → the gateway never learns
board1's MAC → forwards nothing. In one capture board1 *did* reply and the whole ping went through.

**Contributing factor found — RX overload.** board1 + board2 had **no TX-power cap** (~27 dBm) while
the gateway caps to 1 dBm. Centimeters apart that is classic RX overload (RSSI ~-3 dBm → loss/retries).

---

## Phase 5 — Lowest TX power + static-ARP isolation: ARP is NOT the root cause

**Lowest TX power (answered).** `mmwlan_override_max_tx_power(uint16_t)` treats **0 as "disable the
cap"** (full power), so **1 dBm is the lowest cap the API allows** — already what all three nodes
were set to. Below that needs physical separation, not the API.

**Static-ARP isolation.** Pinned static ARP on **both** sides via `etharp_add_static_entry` (run
inside `esp_netif_tcpip_exec`; needs `CONFIG_LWIP_DHCPS_STATIC_ENTRIES=y` + `#include
"lwip/etharp.h"`): gateway `10.9.9.100→board1`, board1 `10.9.9.136→gateway` + return route. Boot log
confirmed "static ARP pinned".

**Result: STILL 0/10.** `ap_rx` grows, `ap_tx`+`mesh_rx` flat. **Bypassing ARP entirely did not help
— the blocker is NOT ARP resolution.** (All static-ARP + power edits reverted.)

---

## Phase 6 — Decrypt logging: the CCMP crypto/decrypt path is FULLY EXONERATED

**Code check first (no bench).** The MTK is a *derived* per-pair key (`mesh_derive_mtk`,
`umac_mesh.c:1491`): both peers independently `sha256_prf(PMK, "Temporal Key Derivation",
min/max(nonce) || min/max(lid) || AKM || min/max(MAC))`, using `mesh_ctx.mesh_mac` (the correct
**mesh** MAC, symmetric min/max), so gateway-board0 and plain-mesh-board0 derive the *same* MTK.
Derivation is correct → deterministic key bug unlikely.

**Instrument (temporary, reverted): board1 CCMP-RX outcome counters** in
`umac_datapath_sw_ccmp_decrypt` (attempt/nokey/micfail/replay/ok/kid0/kidN), logged in board1's
heartbeat. During a ping board1 reported:
```
att=8  ok=8  kid0=4  mic=0  nokey=0  replay=0
```
**board1 RECEIVES and SUCCESSFULLY DECRYPTS the gateway's forwarded unicast ICMPs** — 4 of them
(`kid0`=pairwise MTK, key_id 0), ZERO failures. **The entire crypto path is exonerated: MTK
derivation, install, and decryption all work.**

---

## Current conclusion — the break is board1's REPLY return-leg

board1 gets the ping and decrypts it fine, but its ICMP **echo-reply never reaches the gateway**
(`mesh_rx` flat), even with static ARP + return route. Structural difference from the working
Phase-1 regression:
- **Regression:** board0→board1 direct; board1 replies to an **on-link** `10.9.9.x` dest.
- **Gateway case:** board1 must reply to an **off-subnet** dest (`192.168.12.x` = board2) via its
  gateway `10.9.9.136` — a different lwIP route, and a board1→gateway mesh-unicast reply that isn't
  arriving.

**Unexplained asymmetry to chase next:** during the ping the gateway RECEIVES board2's AP uplinks
(`ap_rx` grows) but NOT board1's mesh replies (`mesh_rx` flat) — both TX at 1 dBm, same distance, so
not simply RF overload. Either board1 isn't actually TRANSMITTING the reply, or the gateway's
mesh-vif RX drops it.

Delivery note: a board1 deliver-vs-relay counter attempt was **inconclusive** — the `cbnone` probe
was placed before the fallback `data->rx_pkt_callback`, so it fired for the *normal* delivery path
(board1 uses the plain `rx_pkt_callback`, not the VIF_AP ext-cb; mesh frames on a no-AP node demux
to `MMWLAN_VIF_AP` at `umac_datapath.c:930`). Redo it after both callback checks if delivery needs
confirming.

### Next step (fresh session)
Split "board1 doesn't send" vs "gateway doesn't receive" the reply:
1. Add a board1 mesh-**TX** counter (`halow_transmit` / `mmwlan_tx_pkt`) → does board1 emit the
   echo-reply at all?
2. Add the CCMP-RX counters to the **gateway** app (board0's morselib had them but its app never
   logged them) → does board1's reply reach + decrypt on board0?

Everything upstream is proven: lwIP `ip_forward`, the gateway 4-addr TX build, ARP (bypassed via
static ARP), and board1's receive + CCMP decrypt.

---

## Bench gotchas encountered (reusable)

- **`rimba-halow-mesh-ap` is UNTRACKED** in git. `git checkout <file>` cannot revert its edits — and
  passing it alongside tracked files aborts the whole `git checkout` (reverts nothing). Revert by hand.
- **chronium's `morse0` monitor loses data capture after a down/up cycle** — it captures data frames
  only while *first* up; after `ip link set morse0 down` then up it captures **beacons only**
  (confirmed: `ap_rx` proved data frames were on-air that the monitor missed). Don't down morse0
  mid-investigation, or reload the monitor driver (not just the link).
- **chronite `wpa_supplicant_s1g` respawns** (a service restarts it after `pkill`). `wlan1` DOWN is
  sufficient for radio-silence.
- **board2 is PPK2-gated** — `ttyACM4` exists only while `tools/ppk2_hold.py` runs; killing the hold
  or a USB replug drops it. Power off with `ppk2_hold.py off`.
- **Resetting the gateway (board0) drops board2's AP association** + board1's peering; wait ~25 s +
  re-verify `mesh_peers`/`ap_stas`/`estab_peers` before pinging.
- **board2's esp_console can get stuck mid-command** — send `\r\n` + `reset_input_buffer()` before
  each `ping`, or a prior `ping -c N` still runs and swallows the new command.
- **morselib `MMLOG` is NOT on the ESP UART** — instrument with app-visible counters + a getter the
  app heartbeat prints; MMLOG_WRN drops (no-key, MIC fail, "No STA record") are invisible on console.

## Radio-silence at session end (done)
board0/board1/board2 flashed to `rimba-hello`; board2 powered off (`ppk2_hold.py off`); chronium
`morse0`+`wlan1` DOWN; chronite `wpa_supplicant_s1g` killed + `wlan1` DOWN. All temporary edits
reverted (mmhalow.c MESHTX trace + VIF_STA; umac_datapath.c CCMP/mesh counters; board1 gw/tx-cap +
static ARP; both apps' `CONFIG_LWIP_DHCPS_STATIC_ENTRIES`). Tree back to just the validated
`umac_keys.c` fix (+16/−5) on the feature branch.
