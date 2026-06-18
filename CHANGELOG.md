# Changelog

All notable changes to the Rimba protocol specification are documented here.

## Draft 0.28

This draft deepens the custody/mule model, refines the config-convergence and
OTA mechanisms, and adds the IBSS fallback strategy to the development plan.

### Custody & mule protocol (Section 9.5)
- Consolidated custody explainer (Section 9.5.0): what custody protects against, OSI placement (L2 `CUSTODY_REQ`/`CUSTODY_ACK` handshake vs L4 `E2E_CUSTODY_ACK` bundle), full flow diagram, and with-vs-without comparison. **Custody is a Rimba extension, not a BPv7 feature** (BPv7 removed BPv6 custody).
- Handshake OSI placement + full arrival-to-departure sequence diagram (Section 9.5.1a): `MANIFEST`/`ACCEPT`/`ACK`/`CONFIRM` are L2 control signalling (frame types `0xB`/`0xC`/`0x9`); only `DATA` carries an L4 bundle, and `E2E_CUSTODY_ACK` is itself an L4 bundle.
- **Mule contact-window / non-stopping-sweep design (Section 9.5.9): custody assumes the mule NEVER stops** — per-pass partial transfer, confirm-before-delete, multi-pass drainage, ALERT-first. A stop is a *bonus* that widens the window (full drain, bulk transfer). 
- "Departing" corrected: under never-stops there is no mule-decided departure. The relay infers the closing window from the mule's HELLO RSSI trend (RSSI past peak); a mule advertises only genuine mule-side unavailability (storage full / self-update reboot). Section 9.5.5 rewritten accordingly.
- Multiple relays sharing one mule (Section 9.5.10): CSMA-serialized turn-taking, one-session lock with a new `CUSTODY_ACK` BUSY phase, `storage_kb` budget advertising, lossless retain-for-next-pass.
- Complete unified flow (Section 9.5.11): one mule arriving, servicing three relays in turn (R1→R2→R3, storage draining 200→120→60→0 KB, graceful FULL saturation with overflow retained), departing by RSSI-inferred window close, with asynchronous per-bundle E2E-ack closure.
- Corrected delete-on-CONFIRM diagrams/statements (Sections 3.8.6, 9.5.1, and the DTN explainer): releasing a bundle on CONFIRM depends on retention mode (EAGER deletes now / RETAINED keeps until end-to-end ack), not unconditional deletion.

### Development plan (now Draft 0.4)
- **RISK-01 fallback strategy — what if IBSS doesn't work.** Three degrees (init quirks → Continuous mode; works-but-different → topology tuning; no ad-hoc mode → AP-STA / LoRa / 802.11s fallbacks), with the reassurance that only the L2 layer depends on IBSS and the DTN/custody/mule/routing/OTA design survives any fallback.
- RISK-01 — HELLO and discovery under AP-STA (if the AP-STA fallback is taken): HELLO stays at L2 but sheds its discovery role to 802.11 management frames (beacon/probe/association) while keeping its Rimba-state payload; the broadcast model fragments (relay-to-relay peering becomes the open design decision).

### Open issues
- **#16 [MEDIUM]** — cross-relay custody priority: with CSMA turn-taking, the mule cannot prefer one relay's ALERT bundle over another relay's routine telemetry; a deferred refinement runs a MANIFEST-only round then accepts globally ALERT-first.

## Draft 0.27

### Encryption & trust
- True end-to-end encryption via ECIES (Section 10.5), plugged into BPSec as a custom security context (not a replacement for BPSec).
- Deployment-CA trust model defeating MITM key substitution; node certificates derived from the deployment root (Section 7.5.5).
- Key-discovery protocol — KEY_REQ/KEY_RESP, relay key cache, decision tree (Section 7.5).
- Deployment encryption profiles: **HYBRID** (default) and **FULL_TRUE**. Pure-soft profile removed. Backend-bound traffic (`ipn:1.*`, `ipn:2.*`) is unconditionally true E2E in both profiles.
- Per-node keypairs + deployment-CA certificate are **mandatory at provisioning** (Section 3.6.5) — every node is FULL_TRUE-capable from first boot; migration is a config flip; no OTA cert-issuance subsystem.
- Profile scope clarified: "node-to-node" = all field-node-to-field-node **endpoint** payloads (leaf/relay/mule); forwarding (hop-by-hop AES-CCM) and backend traffic are excluded (Section 10.5).
- Cloud endpoint added (`CLOUD_ANYCAST_ID`, `ipn:2.*`) alongside gateway (`ipn:1.*`); EID selects the decryption endpoint; three-tier threat model (Sections 8.11, 10.1, 10.5).

### DTN, custody & configuration
- DTN / store-carry-forward explainer and RFC conformance table (Section 9.0).
- Custody reliability: EAGER vs RETAINED modes, end-to-end CUSTODY_ACK, mule custody timeout/re-delegation, downlink mule delivery (Sections 9.5.7–9.5.8).
- Consolidated custody explainer (Section 9.5.0): what custody protects against, OSI placement (L2 CUSTODY_REQ/ACK handshake vs L4 E2E_CUSTODY_ACK bundle), full flow diagram, with-vs-without comparison. **Custody is a Rimba extension, not a BPv7 feature** (BPv7 removed BPv6 custody).
- Configuration change mechanism (Section 9.9): signed config bundles; `config_version` (ordering) + `config_hash` (integrity); decentralized convergence gossip (every node pushes config to behind peers — no roster, no central campaign); same-version-different-hash anomaly detection.
- Config payload capped at `CONFIG_MAX_BYTES = 256 B` for v1 (single-frame, cacheable; raise in a future version); larger payloads must use the streaming path.

### HELLO & neighbor discovery
- HELLO documented as an L2 link-local broadcast (not a bundle); Trickle adaptation (doubling when stable, reset on change) explained at the definition (Section 7.1).
- `config_version` / `config_hash` carried in HELLO for convergence.

### OTA firmware update (Section 14)
- End-to-end campaign overview timeline (Section 14.0): announce → relay staging → leaf streaming → convergence, with isolated-relay/mule variant.
- Streaming rationale (why firmware is streamed, not bundled), OSI comparison, and streaming sequence diagram (Sections 14.1a–14.1c).
- OSI placement table for all OTA packets (Section 14.1d); unified delivery-mechanism diagram (Section 14.1e).
- OTA frames OTA_START/CHUNK/EOF/ACK/NACK assigned frame types `0xF`–`0x13` in the registry (Section 6.3) and fully field-defined (Section 14.4a).
- **Mechanism A2 — relay-to-relay backbone propagation** (Section 14.6a): the image ripples gateway → relay → relay (store-and-forward); each relay verifies the Ed25519 signature before re-serving, so a bad image stops at the first verifier.
- Mule OTA campaign participation — image library, autonomous delivery, telemetry, self-update timing (Section 14.7a).
- OTA attack-surface analysis (Section 14.13); announce-first verification and wake-budget anti-drain (Section 14.8).
- OTA aligned to the same version (ordering) + hash (integrity) primitive and decentralized "who is behind" comparison as config; image additionally Ed25519-signed.

### Routing (Section 8)
- Multiple-RREP route selection and lost-RREP recovery (Sections 8.5.1–8.5.2).
- RERR precursor-list propagation (Section 8.6).

### Open issues & future investigations
- **#13 [HIGH/URGENT]** — config-changeable parameter scope not yet enumerated (security boundary: provision-only set must exclude `root_secret`, keys, `network_id`, `channel`, channel width).
- **#14 [MEDIUM]** — `target_version` as a signed-announce-backed HELLO hint (deferred).
- **#15 [HIGH]** — OTA for heterogeneous `hw_type`: per-type targeting, mandatory pre-flash hw_type gate (anti-brick), per-type version comparison (deferred).
- **FI-3** — dynamic channel width for short pairwise high-bandwidth bursts (e.g. faster OTA), scoped to one link to avoid network partition; not for v1.

---

Earlier drafts (≤ 0.24) are summarized in the spec's own document-status line and prior design transcripts.
