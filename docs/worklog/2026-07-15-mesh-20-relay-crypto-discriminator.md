# 2026-07-15 — #20 resolved down to a relay-crypto discriminator (not a receiver-host-stack difference)

**Status: RESOLVED** (bench A/B + 5-lens adversarial verification + E1 offline MIC-verify). The 2026-07-14 "#20
reversal" (HW-crypto Linux node *delivers* the multi-hop forward that an ESP *withholds* → "ESP-vs-Linux
receiver-host difference") is **refuted as relay-crypto-confounded**. On a *fixed* HW-crypto Linux receiver, the
crypto mode of the **forwarding relay** decides whether a forwarded frame reaches the receiver's kernel — and E1
shows *why*: an **HW-crypto relay emits a malformed forwarded frame** (its CCMP MIC doesn't verify under the
relay's own key), so any canonical receiver rejects it. A firmware fault in the HW-encrypt-on-forward path, not
a receiver/host difference. No firmware change made; bench radio-silent afterward.

## The question
#20: does a HW-crypto Linux node's kernel *deliver* a multi-hop mesh forward (mesh-SA ≠ TA), or withhold it
like the ESP appeared to? The prior two verdicts disagreed:
- 07-11: "universal wall — Linux also withholds (raw-socket = 0)".
- 07-14 "reversal": re-ran with the shipping-default relay and Linux *delivered* (raw-socket 223, app 99%) →
  "ESP morselib withholds, Linux morse_driver delivers = a receiver-host-stack difference".
- An RX-side source hunt (morselib vs morse_driver) found no receiver config difference.

The uncontrolled variable, in hindsight: **07-11 ran the ESP mesh in HW-crypto (`g_mesh_sw_crypto=false`),
07-14 in SW-CCMP (the shipping default, `=true`).** So "re-ran the EXACT rig" was false on the decisive axis —
the *relay's* crypto mode.

## Method
- Rig: `board1(origin) → board0(relay) → chronite(Linux receiver)`, forced line via ESP peer allowlists
  (board1 peers ONLY board0, so board1's frames reach chronite only as a forward: mesh-SA=board1 ≠ TA=board0).
  App mode `MESH_CHRONITE_LINE` (added + reverted). board2 absent (radio-silent).
- chronite = Pi5 + MM6108, morse_driver, **native HW-crypto verified** (`no_hwcrypt=N` AND `thin_lmac=N`), mesh
  MAC `3c:22:7f:37:51:38`, SAE `rimbamesh2026`, ch27. **NOT recompiled between conditions** — the only thing
  that changes A→B is the ESP relay's `g_mesh_sw_crypto`.
- chronite kernel-RX counter: raw `AF_PACKET` on `wlan1`, skip `PACKET_OUTGOING`, tally by 802.3 src MAC.
  mac80211 delivers a mesh data frame to `wlan1` as 802.3 with src = original mesh SA, so **src=board1 at
  chronite ⇒ a forward its kernel received.**
- **Positive control** (essential): chronite simultaneously pings board0 (its DIRECT peer, SA==TA). Counting
  board0-direct frames proves the counter is live AND that chronite delivers direct frames — so a `0` for
  board1 is a real drop, not a dead instrument.
- On-air ground truth: chronium `morse0` passive monitor on ch27 (`~/halow-mon.py`) decoded every relay TX.

## The 2×2 (does the receiver's host/kernel receive the FORWARD?)
| Relay crypto | Receiver | Forward delivered? | Evidence |
|---|---|---|---|
| SW-CCMP | ESP board2 | **YES** | morselib RX-entry counter fwd_uni 0→36, fwd_grp=6; board1 88/89 replies |
| HW-crypto | ESP board2 | **NO** | fwd_uni=0, fwd_grp=0 / 40s; 36 forwards on-air; board2 peered (direct RX OK) |
| SW-CCMP | Linux chronite | **YES** | kernel RX 33 ICMP + 2 ARP src=board1; board1 32/32 replies; pos-ctrl 28/28 |
| HW-crypto | Linux chronite | **NO** | kernel RX 0 src=board1; 31 forwards on-air; pos-ctrl 28/28 |

The **chronite rows (3 vs 4)** are the clean single-variable cell (receiver fixed at native HW; only the ESP
relay flips). SW relay → delivered; HW relay → 0 despite 31 forwards on air and a passing positive control.

### Bench-conformance re-run — relay = board2 (the fully-wired ESP)
The rows above used **board0** as the relay, but the bench doc requires a properly-wired ESP relay to be
**board2** (board0/board1 lack the WAKE/BUSY handshake → relay-under-load is a documented wiring artifact). Re-ran
the chronite A/B with **board2 in the relay role** (board1 origin, board0 radio-silent), all else identical:
| Relay = **board2** | chronite kernel RX from board1 | board1 replies | on-air |
|---|---|---|---|
| HW-crypto | **0** | 0/31 | board2 TX'd **30** forwards (TA=board2, A3=board1); pos-ctrl 28/28 |
| SW-CCMP | **33** (31 ICMP + 2 ARP) | **31/31** | (delivered); pos-ctrl 28/28 |
**Identical result with the fully-wired relay** → the board0-wiring residual is eliminated; the relay-crypto
discriminator is not a board0 artifact. chronium monitor was tuned with an explicit `iw set freq 5560` this run.

### E1 — offline CCMP-MIC-verify → the mechanism is a RELAY-TX firmware fault (malformed forward)
The A/B above shows *that* an HW-relayed forward isn't delivered; E1 shows *why*. Printed board2's own MGTK
(group-TX key) at AMPE install (`mmwlan_dbg_own_mgtk`, mangler-safe global), pinged a **nonexistent** IP so
board1 perpetually broadcasts ARP → board2 continuously GROUP-forwards it (encrypted under board2's MGTK),
captured those forwards byte-exact on chronium `morse0`, and MIC-verified them offline against board2's MGTK
using a Python port of morselib `ccmp.c:mesh_ccmp_aad_nonce` (canonical mac80211 AAD/nonce):

| Relay mode | board2 MGTK (that session) | forwarded group frame MIC (canonical AAD, own key) |
|---|---|---|
| **SW-CCMP** (control) | `c08af877…634f` | **VERIFIES** — plaintext = valid ARP (`aaaa03…0806 0001 0800 0604 0001`); 2/2 frames |
| **HW-crypto** (test) | `9ca521c9…c046` | **FAILS** (InvalidTag); 3/3 frames |

Same verifier, same frame layout (3-addr QoS-data, key_id=1 MGTK, PN, 4-byte FCS) — only the encryption engine
differs. So the MM6108 FW, HW-encrypting a **forwarded** (originated-elsewhere) group frame, emits bytes that
are **not canonical-CCMP-valid under the relay's own group key** → any canonical receiver (Linux mac80211, or
another MM6108) rejects it. **Mechanism = relay-TX firmware fault, NOT a receiver-side gate.** This explains
every prior "receiver withheld" reading (07-11 / 07-14 / the 07-15 board2 counter) as downstream of this one
bug; morselib/morse_driver RX is fully exonerated. The verifier's positive control (SW frame verifies → valid
ARP) proves the AAD/nonce construction is correct, so the HW failure is the frame, not the tool. Verified for
the group/MGTK forward; the unicast/MTK forward is presumed the same FW path (not separately checked). Reusable:
`tools/mesh_ccmp_verify.py`, `tools/mesh_grab_fwd.py`. Fix (if HW mesh forwarding is ever wanted) is in FIRMWARE.

## Verdict (calibrated by a 5-lens adversarial pass)
**STRICTLY PROVEN:** for a fixed HW-crypto receiver, the **forwarding relay's crypto mode is decisive** — an
SW-CCMP-re-encrypted forward is delivered, an HW-crypto-re-encrypted forward is not. This **refutes both** the
07-11 "universal wall" and the 07-14 "ESP-vs-Linux receiver" readings; **the 07-14 reversal was
relay-crypto-confounded.**

**NOT proven (retracted over-claims):**
1. *"Receiver host stack is irrelevant / ESP and Linux receivers identical."* The deciding cell **SW-relay →
   HW-crypto-ESP receiver** is untestable under the single global `g_mesh_sw_crypto` and was never run. So the
   same-day "board2 FW gates SA≠TA / morselib exonerated" read is **also relay-confounded** (board2 *did*
   deliver a SW-relayed forward, row 1). And chronite (row 3) shows a Linux HW receiver has **no** SA≠TA gate,
   so the two receivers may in fact *differ*.
2. *"MM6108 HW-crypto TX can't re-encrypt a forward"* — was "likely but unmeasured" when first written; **now
   PROVEN by E1 above** (the HW-relayed group forward's MIC fails under the relay's own MGTK). Relay-TX-malformed.
3. The failing HW-row forwards were **GROUP-addressed** (A1=ff:ff:ff, relay MGTK), not the pairwise unicast
   A4≠TA the literal #20 asks about — that case never got on-air (ARP never resolved in the HW rows). E1 verified
   the group/MGTK case; the unicast/MTK forward is presumed the same FW path (not separately MIC-checked).

## Net (after E1)
The mechanism is settled: an **HW-crypto relay emits a malformed forwarded frame** (bad CCMP MIC under its own
key), so any canonical receiver rejects it — a firmware fault, not a receiver-host-stack difference. The
receiver identity (ESP vs Linux) never mattered. **morselib/morse_driver RX exonerated.** Ship SW-CCMP
(`g_mesh_sw_crypto=true`). Optional deeper localization (not done): AES-CTR-decrypt the HW ciphertext ignoring
the MIC — sensible ARP ⇒ wrong-MIC/AAD only; garbage ⇒ wrong plaintext/key/nonce. Superseded original plan:
1. **Offline CCMP-MIC-verify** the captured HW-relayed group forwards: reflash board0 with a one-line MGTK print
   at AMPE install, capture its forwarded bytes on chronium + the key, feed into the repo CCM/RFC-3610 KAT
   verifier. MIC verifies ⇒ relay re-encryption is correct ⇒ drop is receiver-side. MIC fails ⇒ relay TX
   malformed (with the exact sub-fault). Zero new radio runs beyond the capture.
2. **De-globalize** `g_mesh_sw_crypto` (per-role) and run **SW-relay → HW-crypto-ESP receiver**: delivered ⇒
   kills the board2 SA≠TA gate (truly relay-only); dropped ⇒ the ESP receiver genuinely gates (ESP ≠ Linux).

**Practical answer UNCHANGED:** SW-CCMP forwarding works end-to-end for all multi-hop; HW-crypto forwarding
does not, for any receiver. Keep `g_mesh_sw_crypto=true` (shipping default). HW-crypto mesh forwarding is a
future optimization gated on the mechanism above.

## Gotchas (reusable)
- `chronite`/`chronium` `pgrep wpa_supplicant_s1g` silently matches nothing (proc name > 15 chars → needs
  `pgrep -f`), and `pgrep -f "wpa_supplicant_s1g"` **self-matches the shell running your script** and kills it.
  Target by executable path instead: iterate `/proc/*/exe`, match `*/wpa_supplicant_s1g`, check the cmdline for
  `wlan1`, then `kill` — never touches the wlan0 supplicant.
- morselib debug symbols the app must read need an `mmwlan*`-prefixed name or the librarymangler renames them
  (`-p mmwlan* -p mmhal* …`) and the app link fails with "undefined reference".
- The ping-based rig cannot produce a *unicast* forward in HW-crypto mode: the forwarded broadcast ARP is
  itself dropped at the receiver, so ARP never resolves and no unicast is ever sent. Measure the **group**
  forward (already on-air) or use static ARP + a forced mpath.
