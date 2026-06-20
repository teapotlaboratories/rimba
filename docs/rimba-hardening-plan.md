# Rimba Protocol — Hardening and Spec Maturation Plan

**Companion to**: rimba-protocol-spec.md Draft 0.26
**Document version**: 1.0
**Purpose**: A prioritised roadmap for hardening the Rimba protocol and specification before production deployment. The current spec is internally consistent and functionally complete for the happy path. This document identifies the gaps that matter for a protocol that will run on physically accessible hardware in adversarial outdoor environments.

---

## 0. Framing

"Consistent" and "hardened" are different properties. The spec currently defines what nodes do when things go right. A hardened spec defines what they do when things go wrong — and adversaries live in the failure paths.

This plan is organised into four tiers by priority, preceded by a foundational task (the threat model) that should be completed before the rest, because every hardening decision flows from it.

```
Tier 0   Threat model            ← foundational, do first
Tier 1   Security failure paths  ← highest leverage
Tier 2   Resource exhaustion/DoS ← small additions, large payoff
Tier 3   Core protocol edge cases ← correctness in real deployments
Tier 4   Operational hardening   ← long-term field survival
```

---

## 1. Tier 0 — Threat Model (foundational)

**Do this before any other hardening work.** The spec currently implies a security model (PSK + X25519 ECDH + BPSec) but never states the threat model explicitly. Without it, hardening will be uneven — some paths over-defended, others missed entirely.

The threat model section should explicitly answer, for each adversary class, what Rimba defends against and what it does not:

| Adversary class | Access | Rimba's current defence | Gap to close |
|---|---|---|---|
| Passive eavesdropper | RF sniffing only | BPSec E2E + AES-CCM link crypto | Plaintext metadata leaks — see 1.2 |
| Active injector | Can transmit, no keys | MIC/auth rejection on every frame | Define rejection behaviour explicitly (Tier 1) |
| Compromised single relay | Holds one node's link keys | Partial — BPSec payload stays encrypted | **Largest open question — see 1.1** |
| Physical node capture | Extracts keys from flash | NVS AES-256 encryption | Define key zeroisation, tamper response |
| Malicious mule | Claims custody, drops bundles | None currently | Custody authentication (Issue #9) |
| Insider with root_secret | Has deployment master secret | None — full compromise | Define blast radius + recovery path |

### 1.1 The compromised-relay problem

In a physically accessible outdoor mesh, an attacker will eventually obtain one node's keys. This is the most important threat to reason about. The threat model must define:

```
What a compromised relay CAN do:
  - Read/modify traffic it forwards at the link layer
    (but NOT read BPSec-encrypted bundle payloads — those are E2E)
  - Inject false routing (RREP spoofing, OGM poisoning)
  - Drop traffic (blackhole) or selectively forward (greyhole)
  - Claim false custody as a mule
  - Replay captured frames

What it CANNOT do (if BPSec holds):
  - Read application payloads (encrypted to gateway, not relay)
  - Forge bundles from other sources (lacks their bundle keys)
  - Impersonate the gateway to cloud (lacks gateway uplink creds)

Containment goal:
  A single compromised relay degrades but does not defeat the network.
  Define the blast radius precisely and the detection/recovery path.
```

### 1.2 Metadata and traffic-flow confidentiality (open IBSS)

The Tier 1 backbone runs on an **open** 802.11ah IBSS link — the MM6108 firmware
will not register adhoc stations for chip-level RSN (`SET_STA_STATE` returns -116
on the ADHOC interface), and the spec's security model deliberately sits a layer
up (per-peer ECDH + AES-128-CCM shim, §7/§10). The consequence: the peer-link
shim protects **payload confidentiality and integrity**, but the **entire control
plane and all link metadata travel in clear**. This is the "no plaintext metadata
leaks" gap for the passive eavesdropper, made explicit.

**What an outsider sees today (open IBSS + peer-link shim):**

```
Hidden  : DATA payloads (link AES-CCM + E2E BPSec)
Clear   : RF presence, channel, RSSI, timing      → localization (direction-finding)
          all 802.11 headers: MAC addrs, type, size, seq
          beacons: SSID = "hmdp-<relay_node_id>"  → node_id broadcast in clear
          EtherType 0x88B5                         → protocol fingerprint
          mesh frame_type (in CCM AAD)             → control-vs-data classification
          HELLO (broadcast, pre-link, plaintext)   → node_id, role (gateway/relay/
                                                      mule), peer graph, battery,
                                                      tx_power, TDMA wake schedule
          PEER_OPEN/CONFIRM (pre-key)              → handshake observable
          nonce fields (src node_id, seq, ts, ctr) → cleartext by necessity
```

Net: an attacker who never decrypts a byte can map the full mesh, identify the
**gateway** (highest-value target), physically locate nodes, read each node's
**sleep schedule** (→ efficient targeted jamming during wake windows), and do
traffic analysis. The spec's stated model (spec §10.1) claims data confidentiality
+ integrity, **not** topology/traffic-flow confidentiality — so this is consistent
with the spec, not a violation. It only becomes a gap **if hiding topology, roles,
gateway identity, or wake schedules enters the threat model.**

**Core mitigation — a network group key for the broadcast control plane.** HELLO,
routing (RREQ/RREP/RERR/OGM/TIME_SYNC), and the PEER handshake leak because they
are broadcast or pre-link and cannot use a per-peer session key. But every node
shares the deployment root, so derive a network-wide key and AES-128-CCM the
control plane under it:

```
network_key = HKDF(deployment_root_secret, salt = "HMDP-v1-netkey")
```

This is the Thread / 802.15.4 model (network key for the control plane + per-link
keys for data) and is the single highest-leverage change. Tradeoff: the control
plane gains no forward secrecy (static shared key — rotate periodically), but
topology/roles/gateway/TDMA become opaque to outsiders. All software (mbedTLS),
same primitives as the peer-link shim.

**Per-leak mitigation (in priority order):**

```
Leak                                Mitigation                         Residual
──────────────────────────────────────────────────────────────────────────────
HELLO topology/role/peers/TDMA      encrypt with network_key + drop    none (outsider)
                                    non-essential fields
PEER_OPEN/CONFIRM observable        wrap in network_key                "two MACs talk"
routing control (RREQ/RREP/OGM)     network_key                        none (outsider)
SSID = hmdp-<node_id>               opaque deployment-wide SSID;        "a net exists"
                                    never embed node_id
frame_type in AAD (ctrl vs data)    move frame_type into ciphertext    inferable by size
MAC addresses (clear in header)     rotate MACs over time (identity    short-term linkable
                                    lives encrypted at mesh layer)
frame sizes / volume                pad to fixed size buckets          coarse volume
EtherType 0x88B5 fingerprint        (low value to change)              "custom L2 exists"
```

**Unavoidable residuals (cost exceeds benefit / power budget):**
- **RF presence + localization** — any SDR can direction-find a transmitter. Only
  physical/operational mitigations help: adaptive/min TX power (already specced),
  fewer/less-frequent beacons + HELLOs.
- **Coarse traffic analysis (timing/volume)** — the real defences (constant-rate
  cover traffic, heavy padding) fight the µA-class battery budget head-on and are
  largely a non-starter. Size-bucket padding is the affordable partial measure.

**Recommendation / sequencing.** If topology privacy is in scope: (1) network group
key on the whole broadcast control plane — earns most of the protection for the
least cost; (2) opaque SSID — trivial; (3) `frame_type` into ciphertext + size-
bucket padding — cheap; (4) MAC rotation — only if anonymity vs. a persistent local
observer matters (it interacts with the per-peer records and reachability); (5)
accept RF localization + coarse traffic analysis as residual. **Decide topology
privacy in/out of scope as part of the Tier 0 deliverable before Phase 2 (link
security) implementation begins** — the group key is cheapest to design in up front.

**DECISION (2026-06-19): topology privacy is IN scope.** Phase 2 link security is
a **two-tier** model: a network group key (Tier A) encrypting the full control
plane (HELLO, routing, PEER handshake) for metadata privacy, plus per-peer ECDH
session keys (Tier B) on unicast DATA. Same pairwise+group shape as 802.11 RSN,
relocated into a software shim above the open IBSS link. Accepted residuals: the
group key is outsider-only (a captured node exposes it — see 1.1), and RF
localization + coarse traffic analysis remain; rotate the group key to bound the
capture blast radius. This supersedes the earlier "chip CCMP via SET_STA_STATE /
ibss_rsn" target, which is the wrong layer and blocked on -116.

**Deliverable**: New spec Section 16 (Threat Model) or a dedicated security architecture document.

---

## 2. Tier 1 — Security Failure Paths (highest priority)

The spec defines the happy path for crypto handshakes but is thin on failure handling. This is the single biggest hardening gap.

### 2.1 Handshake and crypto failure handling

Each of these needs an explicit, defined behaviour in the spec:

```
Failure                          Required spec definition
──────────────────────────────────────────────────────────────────
PEER_CONFIRM MIC failure         Retry count, backoff, blocklist
                                 threshold, log behaviour

Bundle BPSec decrypt failure     Drop silently vs alert vs counter;
                                 never leak which key failed

PEER_OPEN replay                 Replay window definition; nonce
                                 cache size and lifetime

PEER_CLOSE forgery               Authenticate PEER_CLOSE so an
                                 attacker cannot tear down links

Malformed frame (any type)       Parse limits, max field sizes,
                                 reject-and-count, never crash

Repeated auth failure from peer  Per-source failure budget →
                                 temporary blocklist with decay
```

### 2.2 Routing security (RREP / OGM spoofing)

This is the classic mesh attack and deserves its own treatment. A malicious node claims "I have a route to the gateway," attracts traffic, then blackholes it.

```
Attacks to defend against:
  - RREP spoofing: false "I can reach the gateway" claims
  - OGM poisoning: false originator metrics to attract traffic
  - Sequence number manipulation: claiming impossibly fresh routes
  - Route blackhole / greyhole: attracting then dropping traffic

Candidate defences (to evaluate and specify):
  - Authenticated routing messages (sign RREP/OGM with link key)
  - Gateway anycast responses signed with root-derived key
  - Path validation: end-to-end ACK confirms route actually works
  - Anomaly detection: relay forwarding ratio monitoring
```

**Note**: DTN provides partial natural resilience here — if a route blackholes, the bundle's lack of delivery confirmation eventually triggers re-discovery, and DTN buffering means data isn't immediately lost. But the spec should make this explicit and add active defences.

**Deliverable**: Expand spec Section 10 (Security) with a failure-handling subsection, and add routing-security rules to Section 8.

### 2.3 OTA signing-key and verification integrity (catastrophic if broken)

The OTA image signature (spec Section 14.10) is the single most critical
security primitive in the system: if it breaks, an attacker achieves
fleet-wide code execution — strictly worse than any data compromise. This is
spec Section 14.13 Vector 4, elevated here to Tier 1.

```
Failure / attack                 Required defence
──────────────────────────────────────────────────────────────────
Signing key leak                 ESP-IDF secure-boot eFuse (key in
                                 hardware); per-deployment-batch signing
                                 sub-keys to bound blast radius; documented
                                 key-rotation + physical re-flash recovery

Verification implementation bug  Verify the COMPLETE image, never partial;
                                 constant-time signature comparison; no
                                 acceptance of malformed signatures; test
                                 vectors for malformed/edge-case inputs

Verification bypass              No debug/test flag that skips signature
                                 verification in production builds; secure
                                 boot enforces verification at the hardware
                                 level even if firmware logic is bypassed

Rollback to vulnerable version   target_version > current enforced at both
                                 relay (READY gate) and leaf (OTA_START)
```

**Deliverable**: Audit the signature-verification path; mandate secure-boot
eFuse for production; specify per-batch sub-keys and the recovery procedure.

### 2.4 Mule custody authentication (Open Issue #9, raised to HIGH)

A fake mule that accepts custody and silently discards bundles causes data
loss with no trace. Framed as a security failure path, this belongs in Tier 1.

```
Required: authenticate mules before custody transfer — deployment
certificate (the CA model from spec Section 7.5.5 applies directly)
or a pre-provisioned mule-ID allow-list held by relays. A relay must
not transfer custody to an unauthenticated mule.
```

---

## 3. Tier 2 — Resource Exhaustion / DoS

A sparse outdoor mesh is physically accessible. One bad node should not be able to break the network. These are small additions with large robustness payoff.

```
Attack vector            Current state         Hardening to add
──────────────────────────────────────────────────────────────────
RREQ flooding            Gossip limits         Per-source RREQ rate
                         re-broadcast, not     budget (token bucket)
                         origination

Bundle store filling     STORE_EVICT_PCT       Per-source bundle
                         eviction, no source   quota / admission
                         quotas                control

Peer link exhaustion     No max peer count     MAX_PEERS cap +
                                               LRU eviction policy

HELLO storm              Trickle on own        Inbound HELLO rate
                         HELLO, no inbound     cap per source
                         cap

Leaf battery drain       Wake window is        Cap frames accepted
                         bounded               per wake window;
                                               abort on flood

OTA false ota_pending    Announce-first verify  Leaf verifies authenticated
  (battery drain)        + wake-budget cap      relay + signed announce
                         (spec 14.8)            BEFORE extended wake;
                                               OTA_WAKE_ATTEMPT_MAX cap

OTA transfer disruption  CRC-16 (accidental    Wake-budget cap aborts after
  (endless retransmit)   only, not malicious)  OTA_WAKE_ATTEMPT_MAX failed
                                               transfers; report anomaly
```

These map partly to existing Open Issue #4 (RREQ Storm Validation). The validation work there should be extended to include adversarial flooding, not just organic scale. The OTA DoS vectors are specified in spec Section 14.13 (Vectors 2–3) with mitigations already in Section 14.8; Tier 2 work is to validate them under adversarial conditions.

**Deliverable**: New parameters (RREQ_SOURCE_BUDGET, MAX_PEERS, per-source bundle quota) in Section 12.2, with enforcement rules in the relevant protocol sections. OTA anti-drain parameters (OTA_WAKE_ATTEMPT_MAX, OTA_BACKOFF_CYCLES) are already in Section 12.2 / 14.13.

---

## 4. Tier 3 — Core Protocol Edge Cases

Correctness gaps that will surface in real deployments even without an attacker.

```
Edge case                    Problem                Fix needed
──────────────────────────────────────────────────────────────────
Sequence number wraparound   seq is uint16; what    Define wraparound
                             happens at 65535→0?    comparison (RFC 1982
                                                    serial number math)

Reboot vs replay             Reboot resets seq;     Define how neighbours
                             neighbours can't tell  distinguish reboot
                             reboot from replay     (incarnation number?)

Clock going backward         RTC replaced/reset →   Define behaviour when
                             timestamps jump back   now < last_seen_ts

Partition healing            Two diverged halves    Define route table
                             reconnect              reconciliation

Bundle ID collision          Two leaves generate    Define dedup key
                             same bundle ID         uniqueness guarantee

OGM→RREQ transition race     Node switches mode     Define route validity
                             mid-conversation       across transition

Custody mid-transfer crash   Confirm vs delete      Bulletproof ordering;
                             ordering on failure    already partial in 9.5
```

**Deliverable**: An edge-case handling subsection per affected protocol area, plus possibly a new "incarnation number" field to distinguish reboots from replays.

---

## 5. Tier 4 — Operational Hardening

What keeps the network alive over years in the field.

```
Area                  Current state        Hardening to add
──────────────────────────────────────────────────────────────────
OTA firmware update   Mentioned for config Signed images, A/B
                      bundles, not images  partitions, rollback
                                           on bad flash

Key rotation          Out of scope (Issue  Define a rotation path;
                      #7)                  a multi-year deployment
                                           with a leaked key has no
                                           recovery today

Diagnostics/health    store_pct, battery   Health telemetry framework;
                      in HELLO             how does an operator detect
                                           a misbehaving node?

Time source failure   OSF flag detection   Define behaviour when RTC
                                           dies permanently

Graceful degradation  Partial (isolation   Minimum viable behaviour
                      backoff, DTN)        when flash failing, battery
                                           critical, RTC dead
```

**Deliverable**: An operations/lifecycle section; OTA security spec; promote key rotation from "out of scope" to a defined v2 mechanism.

---

## 6. Recommended Sequence

```
Phase H1 — Foundation (do first):
  1. Write threat model (Tier 0)
     → frames every subsequent decision
     → resolves the "compromised relay" blast-radius question

Phase H2 — Core security (highest leverage):
  2. Security failure paths (Tier 1, §2.1)
  3. Routing spoofing defence (Tier 1, §2.2)
  4. Resource quotas (Tier 2)

Phase H3 — Correctness:
  5. Sequence/reboot/clock edge cases (Tier 3)
  6. Partition healing, bundle dedup (Tier 3)

Phase H4 — Field survival:
  7. OTA security (Tier 4)
  8. Key rotation mechanism (Tier 4)
  9. Health/diagnostics framework (Tier 4)
```

---

## 7. Relationship to Existing Open Issues

Several hardening items extend or formalise existing spec Open Issues (Section 14):

```
Open Issue                          Hardening tier
──────────────────────────────────────────────────────────────
#4  RREQ Storm Validation [HIGH]    Tier 2 — extend to adversarial
#7  Key Rotation [MEDIUM]           Tier 4 — promote to defined mechanism
#9  Mule Authentication [MEDIUM]    Tier 1 — custody abuse is a security
                                    failure path, raise priority

New issues this plan would add:
  - Threat model definition [HIGH]
  - Security failure path handling [HIGH]
  - Routing spoofing defence [HIGH]
  - Resource exhaustion quotas [HIGH]
  - Sequence/reboot/clock handling [MEDIUM]
  - OTA firmware security [MEDIUM]
```

Note that Mule Authentication (#9) is currently MEDIUM but, framed as a security failure path (a fake mule causing silent data loss), it arguably belongs in Tier 1 and should be raised to HIGH.

---

## 8. The One Thing to Do First

If only one item is tackled, make it the **threat model (Tier 0)**. Everything else flows from it. The single most important question it must answer:

> When an attacker captures one relay and extracts its keys — which is inevitable in a physically accessible outdoor mesh — what can they do, and how does the network contain and recover from it?

Answer that precisely, and the priorities for Tiers 1–4 will largely define themselves.

---

*Document version: 1.0*
*Companion spec: rimba-protocol-spec.md Draft 0.26*
