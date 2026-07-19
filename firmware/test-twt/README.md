# test-twt — TWT responder (action frame), agreement established

**Status: automated + hardware-verified** (board0=AP, board1=STA, 2026-07-16: associated 3.5 s,
setup request ret=0, **agreement flow 0 → INSTALLED** → PASS).

## Rig — two roles (all `test-*`)

| role | device | app | notes |
|---|---|---|---|
| **ap** (support) | board0 | `test-apsta-ap` | a SoftAP; the MM6108 is a **TWT responder by default** for an AP vif (`umac_interface.c:145`, "responder is default-on for AP"). up-marker `"ap-ready"` |
| **sta** (reporter) | board1 | `test-twt-sta` | associates, sends a mid-session TWT Setup Request, polls the agreement → INSTALLED, emits the verdict |

Light load, so board0/board1 (unwired WAKE/BUSY) are valid here.

## What it proves / does not prove

- **Proves:** the ESP SoftAP accepts a TWT Setup Request action frame and an agreement reaches
  **INSTALLED** (the action-frame TWT path, PR `teapotlaboratories/rimba#15`).
- **Does NOT prove:** the power saving itself — the doze depth needs the PPK2 current ladder, not a
  pass/fail; nor Linux-STA-as-requester interop (recorded as blocked by PMF).

## How the reporter self-verifies (a morselib accessor)

The agreement state (`EMPTY → PENDING_RESPONSE → PENDING_INSTALLATION → INSTALLED`) lives in an
internal morselib struct, not the public API. So a small public accessor was added —
`mmwlan_twt_agreement_installed(flow_id)` in `.../umac/umac.c` (returns 1=INSTALLED, 0=pending,
−1=none) — reachable from the app because the name matches the `mmwlan*` protected-symbols glob.
The reporter polls `mmwlan_twt_agreement_installed(0)` until it returns 1. This is a **discrete
negotiation outcome**, not an RF measurement: either the AP responded and the agreement installed
or it didn't.

## Assertion

- **PASS** = associated **and** `mmwlan_twt_setup_request` accepted **and** the agreement reaches
  INSTALLED within 20 s.
- **INCONCLUSIVE** = accepted + still associated but the agreement stays pending (the AP may not
  have responded) — not a clear code regression.
- **FAIL** = not associated, the setup request rejected, or no agreement staged at all.

## How to run

```sh
make test-t2 TEST=twt
```

## Expected console (STA / board1)

```
TEST|STEP|associate|PASS|connected=1 after 3500 ms
TEST|STEP|twt-request|PASS|mmwlan_twt_setup_request ret=0
TEST|STEP|agreement-installed|PASS|flow=0 installed=1
TEST|RESULT|PASS|TWT agreement flow 0 reached INSTALLED (AP responded to the Setup Request)
```

## Firmware

STA requester+reporter: `firmware/test-twt-sta/`. AP responder: `firmware/test-apsta-ap/`
(the same SoftAP the `ap-sta-ping` test uses — no TWT-specific config needed, the responder is
default-on).
