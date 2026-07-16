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
- ◐ **Mesh-gate (Mesh + AP)** → [`mesh-ap/rimba-mesh-ap-milestones.md`](mesh-ap/rimba-mesh-ap-milestones.md) (TODO section). The 802.11s mesh half is **built (P0–P6b ✅)** and **mesh + AP concurrency on one radio (A3) is ✅ done** (both vifs beacon concurrently; STA→gate→mesh→far-node forwards end-to-end; app `firmware/rimba-halow-mesh-ap`). **The remaining structural item — and the next big task — is the [802.11s Mesh-gate port](mesh-ap/rimba-mesh-ap-mesh-gate-discovery-design.md) (S1–S6, ~12–19 d, approved 2026-07-10, not started):** follow-Linux gate discovery = RANN + IS_GATE + learned MPP, L2-bridged single subnet. Other open items: **RAW** AP-side port, **AID ≥ 64** on-air validation, Linux STA as TWT requester. *(Action-frame TWT now ✅ — [PR #15](https://github.com/teapotlaboratories/rimba/pull/15).)*
- ✅ **802.11s mesh port (P0–P6b)** → [`mesh-ap/rimba-mesh-ap-milestones.md`](mesh-ap/rimba-mesh-ap-milestones.md) (§802.11s mesh point) + worklog [`worklog/2026-06-26-mesh-mpm-peering-frames.md`](worklog/2026-06-26-mesh-mpm-peering-frames.md). An ESP32 joins a Linux HaLow mesh, pings, originates + **relays** multi-hop, group-forwards, and does **PERR** broken-link teardown.
- ✅ **Mesh throughput (P6c partial)** — **airtime metric** ✅ (byte-exact `airtime_link_metric_get` port, 2026-07-02) · **AMPE/SAE + CCMP** ✅ · **iperf throughput test** ✅ · **real per-peer rate control** ✅ (**~2.2×**) · **A-MPDU aggregation S0–S3** ✅ (**~37% single-hop, ~38% relay**; teapotlaboratories/mm-esp32-halow#23 + #33, 2026-07-15). Mesh multi-hop went **~0.03–0.06 → ~0.5–1.7 Mbit/s**; the ceiling is now the **bench RF link (RX overload)**, not the code. **P6c remainder:** RANN/root + proxy/gate (`mpp`) — both are the Mesh-gate port above — and **mesh power save**.
- ☐ **Mesh hw-restart recovery** → mesh-ap backlog. **A mesh node cannot survive any `hw_restart` today** — no `umac_mesh_handle_hw_restarted`, so a chip reset wipes the vifs and the node goes **silently deaf** (bench-proven 2026-07-15, both A/B arms). Prerequisite for revisiting FIX-1 (removed; archived at halow tag `archive/fix1-implementation`).
- ☐ **RAW (Restricted Access Window) — AP-side, port from Linux** → [`mesh-ap/rimba-mesh-ap-milestones.md`](mesh-ap/rimba-mesh-ap-milestones.md) (TODO, RAW item). Big standalone feature: port the RPS IE + AID grouping + slot definition from `morse_driver` `raw.c` (1742 lines) + `page_slicing.c`, follow Linux exactly, write the new-code↔Linux map. **Recon/feasibility pass first** (does the fw accept the RAW config cmd; minimum-viable scope). Its own branch + PR.
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

- ✅ Fixed the pre-existing broken worklog link in `worklog/2026-06-22-mesh-ap-twt.md` (the never-created `2026-06-21-i4-beacon-source-addr-firmware-wall.md` → redirected to `2026-06-21-phase1-validation-complete.md` + dev-plan RISK-02). The `ibss/rimba-ibss-milestones.md:349` mention is honest prose ("referenced but never created"), not a link — left as-is.

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
