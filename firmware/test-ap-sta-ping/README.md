# test-ap-sta-ping — AP ↔ STA association + ping

**Status: automated + hardware-verified** (board0=AP, board1=STA, 2026-07-16: SAE assoc in
11 s, **15/15 ICMP replies**, RTT 10–37 ms, 0 timeouts → PASS).

The first fully-orchestrated multi-board T2 test. The host-side orchestrator
(`tools/regtest/t2_onair.py`) flashes both roles, verifies the AP came up, then reads the STA's
verdict — you run one command and get one pass/fail.

## Rig — two roles

| role | device | app | notes |
|---|---|---|---|
| **ap** (support) | board0 | `rimba-halow-ap` (existing, unmodified) | brings up the SoftAP + static IP 192.168.12.1, answers ICMP; up-marker `"static IP"` |
| **sta** (reporter) | board1 | `test-apsta-sta` (this) | associates (SAE), pins 192.168.12.2, pings the AP, emits the `TEST\|` verdict |

board0/board1 are fine here: this is light load (association + 1 Hz ping), not the sustained
forwarding that needs board2's wired WAKE/BUSY.

## What it proves / does not prove

- **Proves:** an ESP SoftAP accepts an ESP STA's SAE association and IP data round-trips over the
  HaLow link.
- **Does NOT prove:** throughput, power-save, or the AID ≥ 64 ceiling (that needs 64+ associated
  STAs — structurally impossible on a 3-board bench, recorded as unverifiable).

## Assertion strategy — structural, not RF-noisy

Association is the binary structural fact. The reply *count* is RF-bound, so:

- **PASS** = associated **and** ≥ 8/15 ICMP replies (a wide floor).
- **INCONCLUSIVE** = associated but 1–7 replies (a marginal RF link, not a clear code regression).
- **FAIL** = not associated, **or** associated with 0/15 replies (at close bench range the recorded
  link is ~0 % loss, so 0 replies means the IP/ICMP path is broken, not merely lossy).

The observed 15/15 / 0-timeout / 10–37 ms matches the recorded milestones
(`docs/worklog/2026-06-18-halow-ap-sta-ping.md:59-63` ~11–27 ms / 0 timeouts;
`2026-06-23-regression-stress-test.md:21` 33/33).

## How to run

```sh
source vendor/esp-idf/export.sh
python tools/regtest/run.py t2 --test ap-sta-ping     # or: make test-t2 (runs the whole T2 suite)
```

The orchestrator: resolves ap→board0 / sta→board1 by efuse MAC → flashes `rimba-halow-ap`, waits
for its `"static IP"` up-marker → flashes `test-apsta-sta`, captures until `TEST|END` →
records the STA's verdict → flashes `rimba-hello` back to both boards (radio-silent). Exit 0 ⇔ PASS.

If the AP never prints its up-marker within its boot window, the test FAILs early with "the ap
role did not come up" — so an AP-side problem is not misreported as an STA association failure.

## Expected console (STA / board1)

```
TEST|BEGIN|name=ap-sta-ping|rig=...
TEST|STEP|associate|PASS|connected=1 after 11000 ms
TEST|INFO|STA static IP 192.168.12.2, netif up=1
TEST|INFO|reply from 192.168.12.1 seq=1 time=37 ms
...
TEST|STEP|ping|PASS|replies=15/15 timeouts=0
TEST|RESULT|PASS|associated (SAE) + 15/15 ICMP replies from 192.168.12.1
TEST|END|name=ap-sta-ping
```

## Firmware

The STA reporter lives at `firmware/test-apsta-sta/` (registered in the manifest as
`test-apsta-sta`). It reuses `rimba-halow-ap`'s SSID/PSK/subnet and static-IP-on-a-DHCP-netif
pattern. The AP role needs no new firmware — `rimba-halow-ap` already brings up the SoftAP and
answers ICMP.
