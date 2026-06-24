# Rimba — master TODO / roadmap

A single high-level view of outstanding work. **This is an index, not a backlog** —
the detailed, authoritative TODO lists live in the per-area docs linked below.
Update *those*; keep this page to one line per item with a pointer. Status:
✅ done · ◐ in progress · ☐ todo · 🔒 blocked.

**Where we are:** Phase 1 (IBSS foundation) is validated on hardware; we're in the
**L2-decision sub-phase** — building both candidate link layers (IBSS and the
Mesh-gate) to compare before committing.

---

## Now — current focus

### L2 link layer — pick IBSS vs Mesh-gate (build both, compare)
- ◐ **IBSS hardening** → [`ibss/rimba-ibss-milestones.md`](ibss/rimba-ibss-milestones.md) (TODO section). Headline open items: hop-by-hop **CCMP** (#3, 🔒 on the firmware station-handle), **dynamic create-else-join** (#7), beacon contention (#5).
- ◐ **Mesh-gate (Mesh + AP)** → [`mesh-ap/rimba-mesh-ap-milestones.md`](mesh-ap/rimba-mesh-ap-milestones.md) (TODO section). Headline open items: **port 802.11s mesh into morselib** (the big one — blocks an all-ESP32 relay), **AID ≥ 64** on-air validation, Linux STA as TWT requester.
- ☐ **Decision:** choose the L2 after the comparison (or define when each applies). Trade-off table in both milestone docs.

### Power-save (early focus — the battery-leaf model rests on it)
- ✅ **RISK-02** radio cold-boot-to-joined time — measured ≈1.39 s (2026-06-21).
- ☐ **RTC-scheduled "Scheduled mode"** prototype (ESP32 + RTC drives the duty cycle; chip has no IBSS radio PS) → ibss TODO #8; context in [`design-specification/rimba-mm6108-powersave-analysis.md`](design-specification/rimba-mm6108-powersave-analysis.md).

### Cross-cutting
- ☐ **Regression suite** across every built feature (hello / scan / AP-STA / IBSS / TWT / Mesh+AP) so fw/morselib bumps don't silently regress milestones.
- ☐ **Validate PSRAM** memory headroom (RISK-04) → [`rimba-development-plan.md`](rimba-development-plan.md) task 1.8.
- ☐ **Stack bump** (MM6108 fw / morselib / ESP-IDF) — a real port-forward, keep parity with the Linux node → ibss TODO #15.

---

## Next — phased build (above L2, link-agnostic)

The phased plan + per-phase tasks live in [`rimba-development-plan.md`](rimba-development-plan.md) §4.

- ☐ **Phase 2 — Peer links + link security** (Phase-2 crypto shim; pin the SDK, RISK-06).
- ☐ **Phase 3 — Routing & mesh forwarding** (OGM/RREQ, custody-aware).
- 🔒 **Phase 4 — DTN bundle layer (BPv7)** — **RISK-03 BLOCKING**: no ESP32 BPv7 lib; RFC 9171 subset (dev-plan 4.1–4.4).
- ☐ **Phase 5 — Full integration & validation** (incl. adaptive TX power, RISK-05).
- ☐ **Phase 6 — Optional: geographic routing.**

---

## Security & hardening (spans all phases)

Roadmap + tiers in [`design-specification/rimba-hardening-plan.md`](design-specification/rimba-hardening-plan.md); spec issues in
[`design-specification/rimba-protocol-spec.md`](design-specification/rimba-protocol-spec.md) §15–§16.

- ☐ **Issue #13 — config-changeable-parameter scope** **[HIGH / URGENT]** — must be defined before any config-convergence work (spec §15, Issue #13).
- ☐ **Issue #9 — mule custody authentication** **[HIGH]** (hardening §2.4).
- ☐ **Tier 0 threat model → Tier 1 security-failure paths** (highest priority) → hardening-plan; start with "the one thing to do first" (§8).
- ☐ **Tiers 2–4** (DoS / resource exhaustion, protocol edge cases, operational hardening).

---

## Housekeeping

- ☐ Fix the pre-existing broken worklog link: `worklog/2026-06-22-mesh-ap-twt.md` → `2026-06-21-i4-beacon-source-addr-firmware-wall.md` (target never created — find the intended file or drop the link).

---

## Authoritative backlogs (edit these — this page only points to them)

| Area | Doc | What it holds |
|---|---|---|
| IBSS L2 | [`ibss/rimba-ibss-milestones.md`](ibss/rimba-ibss-milestones.md) | Milestones, Linux maps, fork comparison, **IBSS TODO**, findings |
| Mesh-gate L2 | [`mesh-ap/rimba-mesh-ap-milestones.md`](mesh-ap/rimba-mesh-ap-milestones.md) | Milestones, Linux maps, **Mesh-gate TODO** |
| Phased plan + risks | [`rimba-development-plan.md`](rimba-development-plan.md) | Phases 1–6, risk register, per-phase tasks |
| Security | [`design-specification/rimba-hardening-plan.md`](design-specification/rimba-hardening-plan.md) | Threat model + Tier 0–4 |
| Spec | [`design-specification/rimba-protocol-spec.md`](design-specification/rimba-protocol-spec.md) | §15 Open Issues, §16 Future Investigations |
| Validation results | [`ibss/rimba-ibss-test-plan.md`](ibss/rimba-ibss-test-plan.md) | P0 / I.1–I.5 results |
