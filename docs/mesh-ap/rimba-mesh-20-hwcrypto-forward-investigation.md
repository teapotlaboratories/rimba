# #20 — HW-crypto mesh forwarding: investigation guide

**Status: RESOLVED → BACKLOG (2026-07-15).** Root cause found: an **HW-crypto relay emits a malformed forwarded
frame** (its CCMP MIC doesn't verify under its own key), so any canonical receiver rejects it — a firmware fault,
not a receiver/host-stack difference (§2). **Practical answer is settled: ship SW-CCMP** (`g_mesh_sw_crypto=true`);
HW-crypto mesh forwarding is a firmware limitation. Everything below is **parked/optional** — pick it up only if
HW-crypto mesh forwarding is ever wanted, or for deeper FW-fault localization. Backlog items: **Q2** (ESP-vs-Linux
receiver symmetry — needs a de-globalized crypto flag), **Q3** (unicast/MTK forward — E1 verified group/MGTK only),
and the **CTR-decrypt** sub-fault localization (E1 §decision).

**Purpose.** A self-contained guide to *resume* the #20 investigation without re-deriving anything. It states
the current calibrated verdict, the precise open questions, and ready-to-run experiments (with rig recipes,
code, and decision trees). Session record with the raw numbers:
`docs/worklog/2026-07-15-mesh-20-relay-crypto-discriminator.md`. Compressed status: memory
`mesh-20-hwcrypto-forward-host-stack`. Prior chapters: worklogs `2026-07-11-mesh-20-*`, `2026-07-14-mesh-esp-linux-multihop-stress`.

---

## 1. The question, in one line
On the MM6108 (same FW on ESP morselib and Linux morse_driver), can a node **forward** an 802.11s mesh frame
(mesh-SA ≠ TA) such that the next hop's host actually receives it — and if not, **where** does it die?

## 2. Current calibrated verdict (2026-07-15)

**PROVEN (clean single-variable A/B, adversarially verified):** holding a receiver *fixed* at native
HW-crypto (Linux `chronite`, not recompiled), the **forwarding relay's crypto mode decides delivery**:
- SW-CCMP relay → forward delivered to the receiver's kernel.
- HW-crypto relay → **0** delivered, despite ~30 forwards captured on-air and a passing positive control.

This **refutes** both earlier verdicts and identifies the confound: the 07-11 "universal wall (Linux=0)" ran a
**HW relay** (its 0 was correct), the 07-14 "reversal (Linux delivers)" ran a **SW relay**. The "reversal" was
relay-crypto-confounded — it was never an ESP-vs-Linux receiver difference.

**✅ Q1 (mechanism) — ANSWERED 2026-07-15 (E1): (a) the relay produces a MALFORMED re-encrypted frame.** Offline
CCMP-MIC-verify of board2's forwarded GROUP frames against its own printed MGTK (canonical mac80211 AAD): the
**SW-relayed** forward MIC-VERIFIES (plaintext = valid ARP; positive control for the verifier), the
**HW-relayed** forward MIC-FAILS (3/3 HW fail, 2/2 SW pass, same verifier). So the MM6108 FW HW-encrypt-on-
forward emits non-canonical-CCMP bytes under the relay's own key → any canonical receiver rejects it. NOT a
receiver-side gate. See worklog + `tools/mesh_ccmp_verify.py`. (Verified for group/MGTK; unicast/MTK presumed same.)

**Still-open (lower priority):**
- **Q2 (receiver symmetry).** Does an **HW-crypto ESP receiver** drop a **SW-relayed** forward (⇒ ESP and Linux
  receivers genuinely differ; the 07-15 "board2 gates SA≠TA" gate is real) or deliver it (⇒ receiver-
  independent; that gate was relay-confounded too)? Untestable under the single global crypto flag → never run.
- **Q3 (unicast vs group).** The failing HW-row forwards were all **group** (broadcast ARP, relay MGTK); the
  literal #20 is a **pairwise unicast A4≠TA** forward, which never got on-air in HW mode because ARP stalls
  first. Assumed to behave the same; unverified.

**Practical answer (stable, not blocked on the above):** `g_mesh_sw_crypto = true` (SW-CCMP, shipping default)
makes all multi-hop forwarding work; HW-crypto forwarding does not, for any receiver. HW-crypto mesh is a future
optimization gated on Q1.

## 3. The 2×2 (reference data)
| Relay crypto | Receiver | Delivered? | Key evidence |
|---|---|---|---|
| SW-CCMP | ESP board2 | YES | morselib RX-entry fwd_uni 0→36, fwd_grp=6; 88/89 replies |
| HW-crypto | ESP board2 | NO | fwd_uni=0, fwd_grp=0 / 40s; 36 forwards on-air; peered (direct RX OK) |
| SW-CCMP | Linux chronite | YES | kernel RX 33 ICMP + 2 ARP src=board1; 32/32 replies; pos-ctrl 28/28 |
| HW-crypto | Linux chronite | NO | kernel RX 0 src=board1; 31 forwards on-air; pos-ctrl 28/28 |

Only the **chronite rows** isolate one variable (receiver fixed HW; relay flips). The board2 rows flip the
receiver's crypto too (global flag), so they do **not** isolate relay-vs-receiver — that is Q2.

---

## 4. Experiments to run next (ranked)

### E1 — Offline CCMP-MIC-verify the HW-relayed forward — ✅ DONE 2026-07-15 (answered Q1 = relay-malformed)
**Result:** SW-relayed group forward MIC-VERIFIES under board2's MGTK (plaintext = valid ARP); HW-relayed group
forward MIC-FAILS (3/3). ⇒ relay-TX emits malformed bytes; not a receiver gate. Reusable pieces: `own_mgtk`
print at `umac_mesh.c` install via `mmwlan_dbg_own_mgtk` global; `tools/mesh_grab_fwd.py` (full-MPDU capture on
chronium `morse0`); `tools/mesh_ccmp_verify.py` (Python port of `ccmp.c:mesh_ccmp_aad_nonce`, canonical AAD, tries
FCS 0/4 — the monitor keeps a 4-byte FCS). Force continuous GROUP forwards by pinging a nonexistent IP
(perpetual ARP broadcast) so the frame is MGTK-keyed in both SW and HW. Original plan below (for reference):
**Goal.** Decide relay-TX-malformed vs receiver-side-reject with **no on-device receiver instrumentation.**
**Idea.** Capture a HW-relayed forward's exact bytes on-air + the relay's install-time key, then verify the
CCMP MIC offline. MIC verifies ⇒ the relay's re-encryption is correct ⇒ the drop is **receiver-side** (Q1=b,
mechanism claim refuted). MIC fails ⇒ **relay TX malformed** (Q1=a, with the exact sub-fault: AAD-over-A4, PN,
key-id).
**Steps.**
1. Reflash board0 (relay) with a one-line print of the MGTK (group, key_id=1) **and** the per-peer MTK
   (pairwise) at install time — `umac_mesh.c` key-install path (~`:1627`, the `mmwlan*`-mangler-safe print or an
   `ESP_LOGI` via the app). Also print the PN base if reachable.
2. Bring up the HW-crypto chronite-line rig (§5). Capture board0's forwarded frames byte-for-byte on chronium
   `morse0` (`~/halow-mon.py` already prints the post-header CCMP bytes; extend it to dump the full MPDU).
3. Feed {captured MPDU, key, nonce/PN from the CCMP header} into the repo's CCM verifier — the RFC-3610 KAT /
   `esp_mesh_ccm_ad` path used by the SW-CCMP mesh (see `esp32-mesh-swccmp-bulk-aes` memory + `ccmp.c`). Verify
   MIC over the correct AAD (mesh 4-addr / QoS — mind the A4-in-AAD subtlety from the defrag work).
**Decision.** MIC ok → chase the receiver (Q1=b): look at morse_driver/morselib RX for a replay-counter or
key-index mismatch specific to a forwarded (SA≠TA) frame. MIC fail → chase the relay TX (Q1=a): the FW's
HW-encrypt of a re-injected/forwarded frame is wrong; compare against how a SW-CCMP relay builds the same frame.

### E2 — De-globalize the crypto flag; run SW-relay → HW-crypto-ESP receiver (answers Q2)
**Goal.** Separate the relay-TX fault from an ESP receiver-side gate — the one cell the global flag forbids.
**Steps.**
1. Replace the single `static bool g_mesh_sw_crypto` (`umac_mesh.c:146`) with a per-role choice: e.g. key it off
   the node's own mesh MAC (relay board0 = SW-CCMP, receiver = HW-crypto), or a compile-time `-D`. Keep the
   `umac_mesh_sw_crypto_enabled()` getter honest for both TX and RX paths + the key-offload skip
   (`umac_keys.c:38`).
2. Rig board1(origin)→board0(relay, **SW-CCMP**)→board2(receiver, **HW-crypto**) with the fwd_uni/fwd_grp
   RX-entry counter on board2 (§6). Give board1 a **static ARP + forced next-hop** for board2 so a *unicast*
   forward is actually generated (avoids the ARP stall — see §7), so this also touches Q3.
**Decision.** board2 delivers (fwd_uni>0) → the 07-15 "board2 gates SA≠TA" was relay-confounded ⇒ the fault is
**relay-TX only, receiver-independent** (consistent with chronite). board2 drops → the **ESP receiver genuinely
gates** a *correctly* SW-relayed forward ⇒ ESP ≠ Linux receiver, and morselib RX is back in scope.

### E3 — Force a *unicast* A4≠TA forward on-air in HW mode (answers Q3; folds into E2)
The ping rig can't make a unicast forward under HW crypto: the forwarded broadcast **ARP** is itself dropped, so
ARP never resolves. Give the origin a **static ARP** entry for the far node **and** a static/forced mesh mpath
(next-hop = relay), so it emits a unicast 4-addr forward directly. Then the group-vs-unicast (MGTK-vs-MTK)
question is measured, not assumed. The app already has `g_static_arp_ip/_mac` plumbing
(`app_main.c`) — wire it into the line modes.

---

## 5. Reusable rig recipe — the chronite-line (ESP origin+relay → Linux receiver)
- App mode (add back; it was reverted): `MESH_CHRONITE_LINE` in `firmware/rimba-halow-mesh-perf/main/app_main.c`
  — board0 allowlist `{board1, MAC_CHRONITE}` (relay), board1 allowlist `{board0}` + `g_ping_target="10.9.9.2"`
  (origin). `MAC_CHRONITE = 3c:22:7f:37:51:38`. Guard unused `MAC_B2` out of this mode (`-Werror=unused-const`).
- Flag: `g_mesh_sw_crypto` = `false` (HW relay) / `true` (SW relay). Global today — see E2 to de-globalize.
- Build/flash: `make build APP=rimba-halow-mesh-perf BOARD=proto1-fgh100m`; flash board0=ttyACM0, board1=ttyACM1
  (identify by efuse MAC, ports re-enumerate). board2 stays on rimba-hello.
- **Bench-conformance (CLOSED 2026-07-15):** the first chronite A/B used **board0** as relay, but the bench doc
  requires a properly-wired ESP relay to be **board2**. The A/B was **re-run with board2 in the relay role**
  (board1 origin, board0 radio-silent) and reproduced identically (HW relay → chronite 0 / board2 TX'd 30
  forwards; SW relay → chronite 33 delivered, board1 31/31; pos-ctrl 28/28 both). So the relay-crypto
  discriminator is **not** a board0-wiring artifact. Keep board2 as the relay for any future ESP-relay run. E2's
  de-globalized build makes this natural: board2 = relay (SW-CCMP) *or* receiver (HW-crypto) as the cell needs.
- chronite mesh up: `cp ~/wpa-interop.conf /tmp/; sudo ip link set wlan1 up;
  sudo wpa_supplicant_s1g -B -Dnl80211 -i wlan1 -c /tmp/wpa-interop.conf; sudo ip addr add 10.9.9.2/24 dev wlan1`.
  Verify HW crypto: `no_hwcrypt=N` **and** `thin_lmac=N` in `/sys/module/morse/parameters/`.
- chronium monitor: `sudo ip link set wlan1 down; sudo iw dev wlan1 set type monitor; sudo ip link set wlan1 up;
  sudo ip link set morse0 up` → defaults to ch27 (`morse_cli -i wlan1 channel` shows 915500 kHz). Capture with
  `sudo python3 ~/halow-mon.py <sec> [ta_prefix]` (decodes 3/4-addr TA/A1/A3/A4).

## 6. Reusable measurement patterns
- **ESP receiver, host-RX-entry counter:** a global counted at `umac_datapath.c:umac_datapath_rx_frame`
  **before** `umac_datapath_rx_frame_filter` (so a nonzero count = the FW handed the frame up). Count forwards
  as `type==DATA && from_ds && dot11_get_sa_data(dh) != dot11_get_ta(...)` — split 4-addr (unicast) vs 3-addr
  fromDS (group). **Name it `mmwlan_dbg_*`** or the librarymangler renames it and the app link fails.
- **Linux receiver, kernel-RX counter:** raw `AF_PACKET` on `wlan1`, **skip `PACKET_OUTGOING`**, tally by 802.3
  src MAC. A frame with src = the origin's mesh MAC = a delivered forward (the origin is reachable only via the
  relay). Script: `/tmp/chronite_rx2.py` pattern in the scratchpad.
- **Positive control (mandatory):** while the forward flows, have the receiver exchange a **direct** frame with
  its immediate peer (e.g. ping the relay). A live direct count + a 0 forward count = a real drop, not a dead
  instrument. Every "0" without this is inconclusive.
- **On-air ground truth:** always corroborate the relay's forward TX on chronium `morse0` — a receiver "0" only
  means "gated" if the frame was provably on the air (`verify-onair-chronium-monitor`).

## 7. Gotchas (cost real time this session)
- **ARP-stall confound.** Under HW crypto the forwarded broadcast ARP is dropped at the receiver → ARP never
  resolves → **no unicast forward is ever created.** Measure the *group* forward (already on-air) or use static
  ARP + forced mpath (E3). A naive ping rig silently measures nothing.
- **`pgrep` self-match / name length.** `pgrep wpa_supplicant_s1g` matches nothing (name > 15 chars). `pgrep -f
  "wpa_supplicant_s1g"` **matches the shell running your kill script and kills it.** Target by exe:
  iterate `/proc/*/exe`, match `*/wpa_supplicant_s1g`, check cmdline for the interface, then `kill`. Never
  `killall` (a node's SSH may ride a different s1g interface).
- **Global crypto flag.** `g_mesh_sw_crypto` flips origin+relay+receiver together on an all-ESP rig, so all-ESP
  rows can't isolate relay-vs-receiver. Use a fixed-HW Linux receiver (chronite rows) or de-globalize (E2).
- **Crypto topology when reasoning about keys.** A *group* forward is protected under the relay's **MGTK**
  (key_id=1); a *unicast* forward under the **pairwise MTK**. The delivered controls this session were unicast,
  the failing HW forwards were group — don't reason about a "pairwise key" for a group-frame drop.
- **Ports re-enumerate** (`ttyACM*`): identify each ESP by efuse MAC (board0=…efa4, board1=…f940, board2=…f008).

## 8. What "done" looks like
- Q1 answered (E1): relay-TX-malformed vs receiver-reject, with the specific sub-fault.
- Q2 answered (E2): ESP receiver same as / different from Linux receiver.
- If relay-TX is the fault: the fix is in the FW HW-encrypt-on-forward path (compare a SW-CCMP relay's on-air
  frame byte-for-byte). If receiver-side: the fix is an RX config (morse_driver as the reference, since a Linux
  HW receiver delivers SW-relayed forwards but not HW-relayed ones — pin *why*).
- Until then: ship SW-CCMP (`g_mesh_sw_crypto=true`).
