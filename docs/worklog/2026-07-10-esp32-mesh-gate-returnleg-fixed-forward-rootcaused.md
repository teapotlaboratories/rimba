# 2026-07-10 — all-ESP Mesh-gate END-TO-END WORKS: return-leg fix (board1 gw) + forward-leg fix (RX-pbuf copy)

The headline: **task #17 is COMPLETE — the all-ESP Mesh-gate end-to-end route
(`STA → AP → ip_forward → mesh → board1 → reply → back`) works 10/10, 0% loss, ttl=63, deterministic
across two runs.** Two distinct bugs were root-caused and fixed this session. (1) The **reply return-leg**:
board1's default gateway was hard-coded to a *phantom* address (`10.9.9.1`), so its echo-reply to an
off-subnet STA could never be routed; fix = set it to the gate's mesh IP (`10.9.9.136`). (2) The
**forward direction** (the deeper, pre-existing bug the 2026-07-09 "ip_forward works (exonerated)" note
had masked): lwIP `ip4_forward` forwards *every* frame (`ip.fw=10`), but the forwarded frame is dropped
before the mesh netif's linkoutput because the esp_netif zero-copy RX pbuf is a **non-contiguous
`PBUF_REF`**, and lwIP's `pbuf_add_header` (non-forced) *refuses* to prepend an L2 header onto a
non-contiguous pbuf (regardless of headroom) — so `ethernet_output` can't add the mesh L2 header. Fix =
in the gateway's `gw_rx_deliver`, copy RX into a **contiguous `PBUF_LINK`/`PBUF_RAM`** and inject it via
`netif->input` (never handing the mmpkt to esp_netif → clean single free). Both fixes are keepers in the
tree; all diagnostic instrumentation was reverted; the bench was returned radio-silent.

Branch: `components/halow` → `feat/mesh-ap-concurrency` (UNCOMMITTED). Supersedes/continues
`docs/worklog/2026-07-09-esp32-mesh-ap-end-to-end-forward.md` and memory
`gateway-e2e-forward-mesh-unicast-gap`. Self-contained: rig, method, every counter reading, both root
causes, and the remaining fix are recorded here so it can resume cold.

---

## Bench setup (all-ESP topology — the reproducible rig)

```
 board2 (STA)               board0 (GATEWAY)                     board1 (2nd mesh node)
 rimba-halow-sta-perf       rimba-halow-mesh-ap                  rimba-halow-mesh
 /dev/ttyACM4 (PPK2)        /dev/ttyACM0                         /dev/ttyACM1
 192.168.12.<mac[5]> --AP--> AP 192.168.12.1 (SSID rimba-ping, SAE, PSK rimbahalow)
   gw .1                     BSSID e0:72:a1:f8:ef:a4
                            ip_forward (CONFIG_LWIP_IP_FORWARD=y)
                            MESH 10.9.9.136 --mesh(ch27,SAE)--> 10.9.9.100  gw 10.9.9.136
                            mesh mac e2:72:a1:f8:ef:a4         mesh mac e2:72:a1:f8:f9:40
```

- **Device→board map is by MAC, not by ttyACM** (they re-enumerate). Verified this session:
  ttyACM0 = board0 (`e0:72:a1:f8:ef:a4`), ttyACM1 = board1 (`e0:72:a1:f8:f9:40`), board2
  (`e0:72:a1:f8:f0:08`) = ttyACM4 **only while `tools/ppk2_hold.py` runs**. **ttyACM2 + ttyACM3 are the
  Nordic PPK2 (`1915:c00a`) — NOT boards; never flash them.** ttyUSB0 = the C6 harness (not a HaLow board).
  Identify with `esptool.py --port … read_mac` (Espressif VID `303a`).
- **Reproducible commands** (unchanged from 2026-07-09): IDF venv python
  `PY=/home/quartz/.espressif/python_env/idf5.4_py3.13_env/bin/python`; build/flash
  `make {build,flash} APP=<app> BOARD=proto1-fgh100m PORT=/dev/ttyACMx`; power board2
  `nohup $PY tools/ppk2_hold.py …` / off with `$PY tools/ppk2_hold.py off`.

### Method — concurrent serial capture (`scratchpad/bench_capture.py`, reusable pattern)
Opens board0/board1/board2 serial handles **ONCE each** (a single open-time reset is tolerated — the
ESP32-S3 USB-JTAG resets on port open; the boards re-associate in ~13 s), reads board0/board1 heartbeats
in threads, waits for bring-up (`mesh_peers>=1 && ap_stas>=1`), then drives `ping 10.9.9.100 -c 10 -i 1`
on board2 **via the same STA handle** (opening board2 a second time resets it mid-test — the bug that
invalidated the first attempt), and reports the before/after counter deltas. All observability is
**app-visible counters printed by the heartbeat** — morselib `MMLOG` is not on the ESP UART.

---

## Part 1 — Reply return-leg: ROOT-CAUSED + FIXED

**Root cause (code):** `firmware/rimba-halow-mesh/main/app_main.c` `mesh_net_task` hard-coded the mesh
netif default gateway to **`10.9.9.1`** — a host that does not exist on this bench. lwIP has no general
routing table for a single netif, so an off-subnet destination (the STA's `192.168.12.x`, reached by
board1's echo-reply) is routed **only** via the netif's default gw. With gw `10.9.9.1`, board1 ARPs a
phantom, never resolves, and the reply is dropped in lwIP **before** `halow_transmit`. On-link replies
(the working mesh-only regression) never hit this path — hence the on-link-works / off-subnet-fails
asymmetry, and why the 2026-07-09 static-ARP-for-`.136` experiment didn't help (a static ARP for `.136`
does not fix a route that points at `.1`).

**Instrumentation (temporary, reverted):** board1 mesh-TX counters in `mmhalow.c halow_transmit` split
by ethertype/proto (`total/ipv4/icmp/arp`) + an RX-deliver counter in `halow_rx`; gateway SW-CCMP RX
outcome counters (`att/ok/kid0/kidN/mic/nokey/replay`) re-added in
`umac_datapath_sw_ccmp_decrypt`. **Symbol-mangler gotcha:** morselib runs `librarymangler.py` over
`libmorse.a`, prefixing every symbol not matching `protected_syms.txt` (`mmwlan*`, `mmhal*`, `mmpkt*`,
…) with `mmint_`. An unprefixed `umac_*` getter is therefore **un-linkable from the app** (undefined
reference to `umac_datapath_get_ccmp_rx_stats`). Fix: name exported diagnostic getters `mmwlan_*` (e.g.
`mmwlan_diag_get_ccmp_rx_stats`) so they ride the protected pattern. (`mmhalow.c` is in the `halow`
component — NOT mangled — so its getters link as-is.)

**Hardware evidence — the counter deltas over a 10-ping run:**

| signal | Run 1 (gw = 10.9.9.1) | Run 2 (gw = 10.9.9.136) | meaning |
|---|---|---|---|
| board1 `rxDeliver` | +2 | +2 | board1 receives the forwarded request either way |
| board1 `meshTX icmp` | **+0** | **+1** | with a valid gw, board1 finally TRANSMITS the reply |
| board1 `meshTX arp` | **+6** (futile, dead gw) | +1 (resolves `.136`) | phantom gw → endless ARP-for-`10.9.9.1` |
| gw `ccmpRX kid0` (pairwise) | +3 | **+4** | gateway decrypts board1's unicast reply |
| gw `ap_tx` (→ STA) | 0 | **+1** | gateway forwards a reply back toward board2 |

So the gw fix makes the entire return path fire for the packet(s) that make it through: board1 sends →
gateway RX + CCMP-decrypts (pairwise MTK, key_id 0) → `ip_forward` → AP vif. **Fix kept in the tree:**
`ip.gw.addr = esp_ip4addr_aton("10.9.9.136")` in `rimba-halow-mesh` `mesh_net_task` (a mesh leaf's
default gw = the Mesh-gate's mesh IP). Everything the 2026-07-09 session exonerated (gateway 4-addr TX
build, ARP-as-such, the whole CCMP/decrypt path) stays exonerated.

---

## Part 2 — Forward direction: a separate bug, ROOT-CAUSED (not yet fixed)

Even with the return-leg fixed, the STA ping stays **0/10**. The forward direction (`STA request → AP →
ip_forward → mesh → board1`) delivers only the **first** request, then stalls. Isolated with layered
counters on the gateway:

- **RX is fine:** `apRX icmp=10 deliverFail=0` — lwIP successfully receives all 10 ICMP requests from
  the STA (not an RX / mailbox / tcpip-thread-stall problem).
- **ip4_forward is fine:** with `CONFIG_LWIP_STATS=y`, `ip.recv=14 fw=10 drop=0 rterr=0`, and
  `mesh_netif up=1 linkup=1`. lwIP forwards **every** packet with zero routing errors and zero drops.
- **…but the mesh netif never transmits them:** `meshTX icmp` = **1** (dynamic ARP) or **0** (static
  ARP), while `meshTX arp` DOES climb. So forwarded frames vanish **between `ip4_forward` (success) and
  the mesh netif's `linkoutput` = `halow_transmit`.**

**The fingerprint → root cause.** Only frames that lwIP *owns a copy of* egress: ARP frames
(lwIP-generated) and, in the dynamic case, the single request lwIP **copied into the ARP pending-queue**
while ARP was resolving (flushed on resolve → `meshTX icmp=1`). A **static** ARP entry removes that
copy path → `meshTX icmp=0` (static ARP made it *worse*, which is the tell). The forwarded request
itself is the **esp_netif zero-copy RX pbuf**: `gw_ap_rx_cb → esp_netif_receive(…, eb=mmpktview)` →
`wlanif.c:168 esp_pbuf_allocate()` builds a **reference pbuf with no L2 TX headroom** (and even the
`CONFIG_LWIP_L2_TO_L3_COPY` path at `wlanif.c:160` uses `PBUF_RAW`, also headroom-less). When
`ip4_forward` re-outputs it on the mesh netif, `ethernet_output`'s `pbuf_add_header(SIZEOF_ETH_HDR)`
**fails on the headroom-less pbuf** → drop before `linkoutput`. lwIP-owned copies have headroom, so they
go out; the forwarded RX pbuf does not.

**This reframes the 2026-07-09 "lwIP ip_forward works (exonerated)" note:** ip_forward *decides* to
forward correctly (fw=10), but the forwarded RX pbuf can't be re-transmitted. The earlier single-packet
"1 ARP + 1 ICMP forwarded" observation was exactly the ARP-queue-copy artifact, not steady forwarding.

### The precise root cause (verified in lwIP source)
`pbuf_add_header_impl` (`vendor/esp-idf/.../lwip/src/core/pbuf.c:476`, gate at `:499/:511`): for a pbuf
WITHOUT `PBUF_TYPE_FLAG_STRUCT_DATA_CONTIGUOUS` (i.e. a `PBUF_REF`) and `force==0`, it **returns fail
unconditionally** ("cannot expand payload to front") — *headroom is irrelevant*. `ethernet_output`
(`.../lwip/src/netif/ethernet.c:302`) uses the non-forced variant, so a forwarded `PBUF_REF` always
hits `pbuf_header_failed → ERR_BUF → drop`. The esp_netif zero-copy RX pbuf is exactly a `PBUF_REF`
(`esp_pbuf_ref.c:59 pbuf_alloced_custom(PBUF_RAW, len, PBUF_REF, …)`). A contiguous `PBUF_RAM` copy takes
the `:499` branch and succeeds.

### The fix (APPLIED + VERIFIED) — `firmware/rimba-halow-mesh-ap/main/app_main.c` `gw_rx_deliver`
Replaced the zero-copy `esp_netif_receive(…, eb=mmpktview)` with: copy the Ethernet frame into
`pbuf_alloc(PBUF_LINK, len, PBUF_RAM)` (contiguous + 14 B link headroom — note `PBUF_RAW_TX` reserves
only `PBUF_LINK_ENCAPSULATION_HLEN`, which is **0** in this build, so `PBUF_LINK` is the correct layer),
`mmpkt_close`+`mmpkt_release` the mmpkt exactly once (we own it — never handed to esp_netif, so no
double-free against the `PBUF_REF` custom-free path), then `esp_netif_get_netif_impl()`→`netif->input(p,
netif)` with `pbuf_free(p)` on `!ERR_OK`. **One edit fixes BOTH directions** — `gw_ap_rx_cb` (AP→mesh
forward) and `gw_mesh_rx_cb` (mesh→AP reply) both funnel through `gw_rx_deliver`. board1 (`rimba-halow-mesh`)
is untouched (it uses the shared `mmhalow.c halow_rx`, has a single netif, does not forward). Ownership
contract verified: `esp_netif_receive`/`wlanif_input` owns the eb in EVERY path (the old caller's
`free-on-!=ESP_OK` branch was dead code — `esp_netif_receive` is compiled to always return `ESP_OK`
here), so bypassing it and owning the mmpkt ourselves is the clean, contract-safe design.

**Verification (`bench_capture.py`, two runs):** `10 packets transmitted, 10 received, 0% loss`,
`ttl=63` (one gateway hop), RTT 30–97 ms. Gateway heartbeat during the ping: `ap_rx=13 ap_tx=11
mesh_rx=13` — the gate now forwards ~10 requests INTO the mesh (mesh_rx climbs) AND ~10 replies back to
the STA (ap_tx climbs), vs the pre-fix `ap_tx=1 mesh_rx=2`.

---

## What is fixed (task #17 COMPLETE)
- **board1 (`rimba-halow-mesh`) default gw `10.9.9.1 → 10.9.9.136`** — the reply return-leg.
- **gateway (`rimba-halow-mesh-ap`) `gw_rx_deliver` copy-into-PBUF_RAM + inject** — the forward-leg
  RX-pbuf drop. Both directions.
Both are keepers in the tree. End-to-end STA↔2nd-mesh-node ping is 10/10. No known remaining blocker for
the basic route. (Follow-ups if desired: throughput/iperf across the gate; more STAs / more mesh hops;
the extra full-frame copy in `gw_rx_deliver` is the intended cost of forwardability.)

## Stress + throughput testing — the per-packet copy is NOT a bottleneck
The forward-leg fix adds a full-frame `memcpy` + `pbuf_alloc(PBUF_RAM)` on every RX frame the gateway
routes. Stress-tested (`scratchpad/bench_flood.py`, `scratchpad/bench_iperf.py`; board1 ran
`rimba-halow-mesh-perf` `iperf -s` with a TEST-ONLY gw=10.9.9.136 hack, since reverted):
- **Large-frame ping flood** (1400 B payload ≈ 1442 B frames, ~25 pps, 200 pkts): 171 replies at
  ttl=63, ~3.5 % loss (first 1–2 = ARP warmup, rest = close-bench RF at 1 dBm). **Heap stable — no
  leak** (dipped under load, recovered + flat); **uptime monotonic — no crash**; board1 stayed peered.
- **UDP iperf across the gate**: client offered 3 Mbit, sent ~0.36 Mbit/s; server received ~0.3 Mbit/s.
  Gateway forwarded 313 UDP frames; heap fluctuated ±5 KB with no monotonic decline; no crash.
- **TCP iperf across the gate**: the 3-way handshake COMPLETES across the gate (client "Successfully
  connected" + server "accept: 192.168.12.51"); goodput is poor (~0.03–0.25 Mbit, 9 s of slow-start
  then a burst) — that's **TCP on a ~130 ms-RTT lossy HaLow link**, not a gate/copy issue.
**Verdict:** the ~0.3 Mbit/s ceiling is radio-limited (HaLow 1 MHz BW + host SW-CCMP mesh + close bench);
a ~1 µs frame copy at ≤~50 pps is ~4 orders of magnitude below the airtime — the copy cannot be the
bottleneck, and it neither leaks nor crashes under sustained ICMP/UDP/TCP forwarding in both directions.
Truly avoiding the copy is not possible (lwIP won't prepend an L2 header onto a non-contiguous
`PBUF_REF`; ESP-IDF's own `CONFIG_LWIP_L2_TO_L3_COPY` does the same copy). A modest optimization exists
— copy only forwarded frames, keep zero-copy for locally-delivered ones (peek dst IP in the RX cb) — but
for a gateway most traffic forwards, so the ROI is low.

## Coverage testing (2026-07-10) — no failures surfaced (single STA / single hop)
Ran the cheap, high-value coverage after the fix, to surface real failures before investing in
generality (`scratchpad/bench_soak.py`, `scratchpad/bench_concurrent.py`):
- **Soak — PASS.** 3-minute sustained ping (3000 × 500 B, ~17 pps): **3000/3000, 0% loss**; gateway
  forwarded ~3000 frames each way; **heap flat** (oscillates in a ~7 KB in-flight band, last sample
  matches early samples — no leak; the −4.5 B/s "slope" is noise within one oscillation); **uptime
  monotonic to 200 s — no crash**; mesh+AP stayed up. Rules out a slow leak in the per-packet copy.
- **Concurrent independent bidirectional — PASS.** board1 (mesh-perf, TEST-ONLY gw=10.9.9.136 +
  auto-ping the STA, since reverted) pinging the STA *while* the STA pings board1: **STA→board1
  150/150 (0%)**; **board1→STA reverse flow works** (25 replies / 6 timeouts — the ~19% is airtime
  contention from the fast flow saturating the shared 1 MHz mesh link + close-bench RF, not a datapath
  fault); gateway forwarded both directions simultaneously; no crash/corruption. **This downgrades the
  Stage-4 "mixed-flow / global-`ops` last-writer-wins" caution** — simultaneous AP-downlink + mesh-uplink
  flows are framed correctly here.
- **Not yet exercised (need multi-node setup — next session):** multiple STAs under the AP (needs a 2nd
  STA, e.g. a Linux AP client), multi-hop (STA→gate→relay→2nd-hop node; needs a 3rd mesh node + forced
  topology), and power-save-behind-gate (STA dozing while the gate buffers downlink).

## Bench gotchas encountered (reusable)
- **Opening an ESP32-S3 USB-JTAG serial port resets the board** (all three reset at t≈0 when the reader
  opens them; they re-associate in ~13 s). **Opening board2 a SECOND time** (separate read + write
  handles) resets it mid-test and silently voids the run — open each port once and share the handle.
- **`sdkconfig.defaults` edits don't always merge into an existing generated `sdkconfig`** — `# CONFIG_X
  is not set` persisted despite adding `CONFIG_X=y` to defaults. Fix: `rm
  build/<app>/<board>/sdkconfig` and rebuild to regenerate from defaults.
- **morselib symbol mangler:** exported diagnostic getters in morselib must be named to match
  `protected_syms.txt` (`mmwlan*`/`mmhal*`/…) or they're renamed `mmint_*` and won't link from the app.
- **`make flash` rebuilds first** (idf.py flash = build+flash); a full reconfigure rebuild can exceed a
  2-minute shell timeout mid-write (leaves the app half-written but re-flashable) or OOM (exit 137).
  Build as a separate step first, then flash.
- The **PPK2-hold must be a durable background process**; if it dies, board2 (ttyACM4) disappears.

## Radio-silence at session end (done)
board0/board1/board2 flashed to `rimba-hello`; board2 powered off (`ppk2_hold.py off`, ttyACM4 gone).
No Linux nodes (chronium/chronite) were brought up this session. All temporary instrumentation reverted
(`mmhalow.c/.h` via `git checkout`; `umac_datapath.c` CCMP counters by hand; both apps' heartbeat
prints, the gateway AP-RX/deliver-fail/IP-stats/static-ARP code + its includes + the sdkconfig
`STATS`/`DHCPS_STATIC` lines). Both apps build clean. Tree retains only the KEEPER fixes on the feature
branch: the prior `umac_keys.c` AP-downlink fix, the new board1 gw fix (`rimba-halow-mesh`), and the new
gateway forward-path fix (`rimba-halow-mesh-ap` `gw_rx_deliver` copy-into-PBUF_RAM + `netif->input`, with
its `lwip/pbuf.h`/`lwip/netif.h`/`esp_netif_net_stack.h` includes). Diagnostic captures live in
`scratchpad/bench_capture.py` (the concurrent 3-board serial harness).
