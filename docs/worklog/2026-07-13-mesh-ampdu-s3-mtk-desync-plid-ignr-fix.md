# Mesh A-MPDU S3 — MTK desync root cause + plid-IGNR fix (2026-07-13)

> **⭐ RESOLUTION / CORRECTIONS (2026-07-13, end of day) — read this first.**
> Two things below turned out wrong; the record is kept for the reasoning trail, but the
> conclusions are superseded:
>
> 1. **The "on-air verify BLOCKED on dead MM6108 / degraded silicon / physical power" thread was a
>    MISDIAGNOSIS.** Root cause: every firmware here was built with bare `idf.py`, which ships
>    `CONFIG_HALOW_COUNTRY_CODE="??"`. morselib (`umac_interface.c:266`) **refuses to bring up the radio
>    when the country is `"??"`** — returns "Channel list not set" *before* touching the chip → version
>    UNAVAILABLE, garbage chip id, MAC `00:00:00`, which is indistinguishable from dead hardware. Building
>    correctly with **`make build APP=… BOARD=proto1-fgh100m`** (→ country `US`) booted every board fine:
>    **chip id `0x0306` (MM6108A1), fw 1.17.8, real MAC, clean SPI read.** All three boards (board0/1/2,
>    incl. board1's swapped module) are healthy. New rule: **always build via `make … BOARD=…`, never bare
>    `idf.py`** (`.ai/AGENTS.md` §Building). Everything in this doc's "BLOCKED / physical / hardware" and
>    "SPI clock / flash size" sections is void.
>
> 2. **The plid-IGNR fix does NOT fix S3.** Re-verified on a proper `make`/US build (multi-hop
>    board0→board2(relay)→chronite): board2 `sw_ccmp_fail=2061/2075` (99.3% MIC fail), unchanged from
>    pre-fix. Dual-side keys still desynced (board0 enc `6f…`, board2 dec `7e…`), and the failure is
>    **immediate from ESTAB, not after a re-peer** — so the plid re-peer guard is the wrong mechanism. It
>    stays as a legit follow-Linux hardening, but the S3 forward-drop is **still open**. New lead: board0
>    `TX-DBG nh_eq_stad=2930` = forwards encrypted with the **wrong per-link key**; single-hop board0→board2
>    worked, so the forward path selects the wrong key — most likely the **destination (chronite) MTK
>    instead of the next-hop (board2) MTK** = the S2 forward `key_stad` override in `umac_datapath`, NOT
>    AMPE derivation. **Confound:** boards too close (−3..−25 dBm) → board0 peers chronite directly and thus
>    has a chronite key to grab; re-diagnosis needs a **forced true-multi-hop topology**. Next S3 task =
>    re-examine the forward `key_stad` selection on such a rig.

## Summary

The S3 relay-forward drop bucket (multi-hop mesh frames dropped at the relay's
SW-CCMP decrypt with a **MIC failure**, not replay) is root-caused to a
**pairwise MTK desync**: the two ends of an established mesh peer link hold
*different* per-pair Mesh Temporal Keys, so every frame one end encrypts fails
the other end's MIC check. The dual-side key dump proved it — board0 encrypts
board2-bound frames with key `6f 1d 9d 9c 48 af d6 49…`; board2 decrypts with
`91 bd e3 64 a0 a4 40 5a…`; the AAD matches on both sides. Same key material
would decrypt; different key material MIC-fails. This is a **key desync, not a
crypto or addressing bug** (AAD + 4-addr header verified to follow mac80211).

Root cause: an established peer that receives a *re-peering* OPEN/CONFIRM (the
peer restarted and re-alloc'd its link id + nonce) **learns the new nonce/plid
into the still-ESTAB peer without re-deriving the MTK**. The MTK is derived once
at ESTAB and never re-derived, so our key stays on the old nonce while the peer
moves to a new one. Fixed by following mac80211's `OPN_IGNR`/`CNF_IGNR`:
ignore an OPEN/CONFIRM whose plid differs from the recorded plid.

## The mesh per-pair key (AMPE MTK)

Secured mesh derives a per-*pair* Mesh Temporal Key at peer-link ESTAB:

```
MTK = sha256_prf(PMK, "Temporal Key Derivation",
                 min(my_nonce, peer_nonce) || max(my_nonce, peer_nonce) ||   // 32 B each
                 min(llid, plid) || max(llid, plid) ||                        // LE16 each
                 AKM(SAE) ||                                                  // 4 B
                 min(my_mac, peer_mac) || max(my_mac, peer_mac))              // 6 B each
```

(`mesh_derive_mtk`, `umac_mesh.c:1527`; matches hostap `mesh_rsn.c`
`mesh_rsn_derive_mtk`.) The derivation is symmetric — both ends feed the same
`min/max`-ordered inputs and get the same 16-byte MTK — **but only if both ends
use the SAME nonce pair and the SAME link-id pair.** The nonce is regenerated on
every `mesh_peer_alloc` (`umac_mesh.c:579` memset then fresh
`mmint_crypto_get_random`); the llid is randomized per alloc (`:592`). So a peer
that frees + re-allocs (restart / re-SAE) comes back with a **new nonce and new
llid** → a **new MTK**.

`mesh_derive_mtk` has exactly one caller: the ESTAB transition
(`umac_mesh.c:1626`, inside `umac_mesh_peer_secure_estab`). It is **never
re-run** while a peer stays ESTAB.

## The desync mechanism (code-level)

Peer-link frame RX (`umac_mesh_handle_peering_action`) processes an
OPEN/CONFIRM in this order, *before* the plink state switch:

1. `peer->plid = peer_plid;`  (`umac_mesh.c:2946`) — overwrite our record of the
   peer's link id, unconditionally.
2. `mesh_process_ampe(...)` (`:2955`) → `memcpy(peer->peer_nonce, …)` (`:1824`) —
   learn the peer's local nonce, unconditionally.
3. `switch (peer->state)` (`:2968`): in `MESH_PLINK_ESTAB` (`:3028`) the only
   action is "retransmit a CONFIRM." **No MTK re-derive.**

So when board0 restarts and re-peers (fresh nonce `N0'`, fresh llid `L0'`), and
board2 is still ESTAB on the old session (`N0`, `L0`, MTK derived from `N0`):

- board2 receives board0's re-peer OPEN → overwrites `peer->plid = L0'` (`:2946`)
  and `peer->peer_nonce = N0'` (`:1824`),
- but stays ESTAB and keeps the **old MTK** (still derived from `N0`).
- board0, having re-alloc'd, now encrypts with an MTK derived from `N0'`.

→ board0's MTK ≠ board2's MTK → every board0→board2 frame MIC-fails at board2.

Why the re-SAE reauth path (`umac_mesh.c:1088`, `MESH_SAE_ACCEPTED` + valid
Commit → `mesh_sae_reauth_free`) doesn't always save us: it fires only if board2
*sees* board0's re-SAE **Commit**. Under multi-hop RX load a dropped Commit +
delivered OPEN leaves board2 adopting the new session identifiers (nonce/plid)
while never re-deriving — the exact window above. Single-hop (stable peering, no
re-peer churn) never hits it, which is why single-hop A/B decrypted 99.4% while
multi-hop relay dropped ~99%.

## The fix — follow mac80211 OPN_IGNR/CNF_IGNR

mac80211 classifies a peering frame *before* the FSM
(`net/mac80211/mesh_plink.c`, `mesh_plink_get_event`):

```c
case WLAN_SP_MESH_PEERING_OPEN:
    if (!matches_local)                     event = OPN_RJCT;
    else if (!mesh_plink_free_count(sdata) ||
             (sta->mesh->plid && sta->mesh->plid != plid))
                                            event = OPN_IGNR;   // <-- ignore
    else                                    event = OPN_ACPT;
    break;
case WLAN_SP_MESH_PEERING_CONFIRM:
    if (!matches_local)                     event = CNF_RJCT;
    else if (!mesh_plink_free_count(sdata) ||
             sta->mesh->llid != llid ||
             (sta->mesh->plid && sta->mesh->plid != plid))
                                            event = CNF_IGNR;   // <-- ignore
    else                                    event = CNF_ACPT;
    break;
```

Once mac80211 has recorded a plid (`sta->mesh->plid != 0`), an OPEN/CONFIRM with
a **different** plid is IGNORED — no nonce learned, no plid updated, no state
change. mac80211 only re-peers from a **freed** sta (plid reset to 0 in
`mesh_plink_deactivate`/`__mesh_plink_deactivate`, `mesh_plink.c:79`). A genuine
restart therefore recovers by *first* tearing down (SAE reauth / inactivity /
CLOSE) → plid 0 → next OPEN accepted → both ends re-derive a matching MTK.

Ported into `umac_mesh_handle_peering_action`, immediately before the plid learn
(`umac_mesh.c:2946`):

```c
if ((action == WLAN_SP_MESH_PEERING_OPEN || action == WLAN_SP_MESH_PEERING_CONFIRM) &&
    peer->plid != 0 && peer_plid != peer->plid)
{
    MMLOG_INF("MESH peer " MM_MAC_ADDR_FMT " plid mismatch rx=0x%04x ours=0x%04x; ignoring (OPN/CNF_IGNR)\n",
              MM_MAC_ADDR_VAL(sa), (unsigned)peer_plid, (unsigned)peer->plid);
    return;
}
```

Placed before both `peer->plid = peer_plid` (`:2946`) and `mesh_process_ampe`
(`:2955`), so a mismatched-plid frame learns **neither** the new plid **nor** the
new nonce — the ESTAB link keeps its MTK consistent with the peer's *old* key.

### Why recovery still works

- Fresh peer: `mesh_peer_alloc` memsets → `plid == 0` (`umac_mesh.c:579`), so the
  first OPEN passes (`peer->plid != 0` is false) → normal peering.
- Retransmits: same plid → passes → retransmit CONFIRM as before.
- Genuine restart: board0's re-SAE **Commit** in `MESH_SAE_ACCEPTED`
  (`:1088-1114`) → `mesh_sae_reauth_free` → `mesh_peer_free` (`:1024-1028`) →
  next alloc has `plid == 0` → next OPEN re-peers cleanly → both re-derive a
  matching MTK. The guard only blocks the *mid-session nonce/plid swap* that
  desyncs the key; it never blocks a from-scratch re-peer.

This is exactly mac80211's invariant: **re-peer only from a plid==0 (freed) sta;
ignore stale-plid frames on an established plink.**

## Files changed

- `components/halow/.../umac/mesh/umac_mesh.c` — plid-mismatch OPN_IGNR/CNF_IGNR
  guard before the plid learn (`~:2946`). No new state; pure follow-Linux gate.

## Build

`idf.py build` clean (esp32s3, app partition 0x1550 B free — tight, unrelated to
this change; the RTC debug telemetry is the size driver).

## Verification

### Root cause — already proven (prior bench, this fix's basis)

The desync itself is not in question: an earlier dual-side SW-CCMP key dump on
the same multi-hop rig captured board0 encrypting board2-bound frames with MTK
`6f 1d 9d 9c 48 af d6 49…` while board2 decrypted with `91 bd e3 64 a0 a4 40 5a…`,
AAD identical on both sides — a definitive key desync (see the S3 MIC-forward-drop
worklog, 2026-07-12). This fix targets exactly that mechanism.

### On-air re-verify of the fix — BLOCKED on board2 hardware (2026-07-13)

Attempted the dual-side re-verify (rebuilt firmware with the plid-IGNR fix,
`idf.py build` clean; flashed board0 + board2; brought chronite up as the mesh
destination — needed a `morse` driver reload + stale-`/var/run/wpa_supplicant_s1g/wlan1`
socket removal, then joined `rimba-mesh` as a mesh point on 5560 MHz).

**board2's MM6108 would not initialise.** Every boot (across clean PPK2 cold
cycles — `off`, 12 s discharge, `on`) the ESP↔radio SPI link returned garbage:
`Morse chip ID: 0x8200f5e7` then `0x0000`, `firmware version:` garbage,
`Wi-Fi MAC address: 00:00:00:00:00:00`, then
`mmwlan_mesh_start FAILED status=4` ("Channel list not set"). The chip has been
dead since the reflash — board2 RX-DBG stayed all-zero. board0's chip is healthy
(RX'd 145 mesh frames, decrypt_ok=67 in the same window).

Per `docs/reference/rimba-bench-devices.md` (the ⚠ CRITICAL callout): **board2 is
the ONLY properly-wired ESP relay** — board0/board1 have the MM6108 `BUSY` pin
unwired, which causes SPI stalls / hw_restart escalation under forwarding load,
so neither can substitute as the relay. And the dual-side *key* comparison needs
a second ESP that dumps its per-pair MTK over RTC — the Linux nodes can't. So the
verification depends on board2, whose radio is now hardware-dead (matches the
"degraded-chip" bench condition flagged in the S3 read-blocked worklog).

**Retry with the correct power-control procedure (2026-07-13, same session).** The bench doc
(`rimba-bench-devices.md` §"Power-cycling") documents that (a) a pyserial console open warm-resets these
XIAO ESP32-S3s via USB-JTAG, which re-wedges the MM6108, and (b) the fix is a *true VBUS cold cycle*, done
**after** flashing (the flash's own `--after hard_reset` is a warm reset that re-wedges), with the console
left untouched during the run. First attempt violated both (opened the console right after each cold cycle,
re-wedging before observing a clean run). So the whole rig was redone properly:

- board0 cold-cycled via **USB hub VBUS** (`tools/esp_usb_power.py board0 cycle 4`, sudo) — a true power cut.
- board2 cold-cycled via **PPK2** (`ppk2_hold.py` off ~15 s → on) — a true power cut.
- Both flashed with the fix build **first**, cold-cycled **last**, consoles **left alone** during the run.
- chronite held up as a mesh point the whole 90 s; peering polled **non-invasively via chronite's
  `iw station dump`** (no ESP console touched).

Result: **chronite saw zero mesh stations after 90 s.** Reading the RTC boot dumps afterward (the counters
survive the console-open reset): **both** board0 and board2 show the identical garbage chip ID
`0x8200f5e7`/`0x0000`, MAC `00:…:00`, `mesh_start FAILED status=4`, and **RX-DBG `mesh_seen=0`** for the
clean cold-boot run (board0's non-zero counters are the stale hours-old `mesh_seen=145`/6f-key values
preserved in RTC from a prior session, unchanged this run). So neither radio initialised even on a proper
cold boot — the PPK2/VBUS cold cycle does **not** heal them. This is the **degraded-silicon condition
already documented at the end of the prior session** (`mesh-ampdu-bench-read-blocked` memory: "the ESP
MM6108s have DEGRADED over a very long abuse-heavy session… Needs a FRESH bench + cool-down to retest"),
not a warm-reset-wedge and not a mis-applied power cycle. A cool-down / fresh (replaced) ESP hardware is
required to unblock.

**Pristine-baseline test — code fully exonerated; the blocker is physical/environmental (2026-07-13).**
The "degraded silicon" framing was wrong (the same MM6108/FGH100M silicon meshes fine on chronite/Linux).
Two follow-ups nailed it:

1. **Third board.** board1 (independent MM6108, 30 s-discharge VBUS cold cycle) fails byte-identically —
   BCF loads (`mf16858`), `mmwlan_get_version` → status 3 (`MMWLAN_UNAVAILABLE`), MAC `00:…:00`,
   `mesh_start FAILED status=4`. **3/3 boards failing identically ≠ 3 independent HW failures = a common
   cause.** (The garbage "chip ID" — `0x8200f5e7`, `0x8200d052`, `0x0000` across reads — is uninitialized
   struct memory left by the failed `get_version`, not a real chip read; it *varies*, proving that.)

2. **Pristine baseline.** Reverted the submodule to `7fdba0c9` (stashed all uncommitted work) and built the
   committed `rimba-halow-mesh` app (zero edits; `MESH_ID "rimba-mesh"`, chan 27, `MESH_SAE_PASSWORD
   "rimbamesh2026"` — matches chronite). Flashed board0, long-discharge cold-cycled, chronite up. board0
   **did not peer** and its console shows the **identical** failure (BCF `mf16858`, `version info - 3`, MAC
   `00:…:00`, `mesh_start FAILED`). So **pristine committed code — the same baseline that meshed with
   chronite in prior sessions — fails identically now.** This exonerates *all* code: my plid-IGNR fix, the
   liveness fix, FIX-1, the telemetry, everything.

3. **Simplest possible bring-up — STA to a Linux AP (mode-independence check).** Started chronite as an
   S1G AP (`hostapd_s1g` + `hostapd-rimba.conf`: SSID `rimba-ping`, SAE `rimbahalow`, ch27), flashed board0
   with the committed `rimba-halow-sta` app, cold-cycled. board0 did **not** associate, and its console
   shows the **identical** failure in the STA path: BCF `mf16858`, `version info - 3`, MAC `00:…:00`,
   garbage chip ID `0x8200cb9f`, `mmwlan_sta_enable: Channel list not set`. So the firmware-boot failure is
   **mode-independent** — STA and mesh fail the same way, before any app logic. This removes any lingering
   "mesh-specific" possibility.

**Conclusion: the MM6108 firmware-boot failure is a shared physical/environmental factor, not software.**
The host loads the BCF over SPI (basic CMD52 works) but the chip firmware never completes boot — on all 3
boards, both power paths (USB VBUS for board0/board1, PPK2 external rail for board2), committed and
uncommitted code, after cold cycles and multi-minute discharges. Since board0/board1 (USB hub) and board2
(PPK2 supply) fail identically, it is not any single power path — it points to something they share
(the bench supply feeding both, thermal after the long abuse-heavy session, RF, or a connection), which
needs hands on the bench to isolate. Candidate checks: the 5 V source(s) feeding the USB hub *and* the
PPK2 external rail (sag/noise under the MM6108 boot-current surge); a full power-off cool-down of the whole
cluster; antenna/SPI/RESET_N connections.

**This is a bench hardware/environment blocker, not a code issue.** The fix stands on:
(a) the definitive prior root-cause proof above, (b) exact line-by-line
correspondence with mac80211 `mesh_plink.c:1077-1088` (OPN_IGNR/CNF_IGNR),
(c) the verified recovery path (re-SAE `mesh_sae_reauth_free` → `mesh_peer_free`
→ plid 0 → clean re-peer), (d) a clean build.

### Pass criteria for the on-air re-verify (when a wired relay ESP is available)

1. board0 `CCMP-ENC` key (relay-bound) **==** relay `CCMP-DEC` key (same 8-byte
   prefix) across a re-peer.
2. relay RX `decrypt_ok` high, MIC-fail bucket ≈ 0 on forwarded frames.
3. multi-hop ping / iperf recovers to the single-hop decrypt rate.
4. `plid mismatch … ignoring` log appears on a re-peer and is followed by a clean
   re-peer (not a stuck link).

### Bench state after this session (radio-silent, per rule)

board0 + board1 flashed rimba-hello; board2 powered off via PPK2; chronite
`wpa_supplicant_s1g` killed + `wlan1` down; chronosalt/chronogen never brought up.
