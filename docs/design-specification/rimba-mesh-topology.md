# Rimba Protocol — Mesh Topology Analysis and Hardening Guide

**Companion to**: rimba-protocol-spec.md Draft 0.26  
**Document version**: 1.0

---

## 1. Recommended Relay Count

The relay count determines routing mode, coverage area, and operational complexity.

```
Scale         Relays    Routing mode           Coverage (500 m spacing)
──────────────────────────────────────────────────────────────────────
Small         5–20      OGM proactive           2–10 km²
Medium        50–200    OGM + Trickle          25–100 km²  ← sweet spot
Large         200–500   Reactive RREQ/RREP    100–250 km²
Maximum       500–1000  Reactive + GPS        250–500 km²
```

**Sweet spot: 50–200 relays.**

At this scale:
- Each relay naturally has 4–8 backbone neighbours at 500 m spacing → strong natural redundancy
- OGM proactive routing covers the lower end; Trickle suppression manages the upper end without switching to reactive
- Channel utilisation from routing overhead remains below 10%
- No GPS needed for routing — RREQ/RREP handles discovery efficiently
- A 2-engineer team can deploy and commission 100 relays in 1–2 days

Above 200 relays, switch to reactive RREQ/RREP routing and consider GPS-equipped relays for deployments above 500 nodes.

---

## 2. Recommended Leaf Count Per Relay

### 2.1 Practical ranges

| Category | Leaves per relay | Relay avg current (K=6) ¹ | Notes |
|---|---|---|---|
| Conservative | 10–30 | ~14.1 mA | Minimal TDMA overhead |
| Typical | 50–100 | ~14.2–14.3 mA | Good density, comfortable |
| High density | 200–300 | ~14.6–14.9 mA | Still within duty budget |
| Maximum | 900 | ~16.8 mA | Hitting relay duty ceiling |

¹ Scheduled mode (K=6) at the datasheet **26 mA** idle-RX baseline (was 7.2–8.5 mA
at the old 12 mA estimate). The **density conclusions are unchanged** — the limit
is the duty-cycle budget (`RELAY_DUTY_BUDGET 0.70`) and channel capacity, not
absolute current. See `rimba-battery-analysis.md` — confirm idle-RX on hardware.

**Practical recommendation: 50–100 leaves per relay.** Leaf count has minimal power impact with a 1 ppm RTC (leaf windows are 100 ms each), so the constraint is primarily channel capacity, not relay power.

### 2.2 Total network leaf capacity

```
Channel capacity constraint (300-byte bundle, 1 MHz channel):

  Reading interval   Max leaves before channel saturates
  ────────────────────────────────────────────────────────
  1 minute           3,250
  5 minutes         16,250
  15 minutes        48,750   ← sparse-first default
  1 hour           195,000

Standard deployment (500 relays × 100 leaves, 15-min interval):
  Total leaves:     50,000
  Channel usage:    ~26%  ✅  comfortable headroom
```

The channel is the binding constraint at large leaf counts, not the relay hardware. For high-frequency readings (< 5 minutes), reduce leaf count per relay or increase the number of relay channels (multi-channel future investigation, Section FI-2 of spec).

---

## 3. Physical Layout

### 3.1 Relay spacing and coverage

```
At 802.11ah 1 MHz bandwidth:
  Open field range:    > 1,000 m
  Semi-urban:            ~500 m
  Urban/obstructed:    200–400 m

Standard relay spacing (semi-urban, 500 m):
  Density: ~4 relays per km²
  Each relay hears 4–8 backbone neighbours directly

  10 relays:  ~2–3 km²
  50 relays:  ~12 km²
  100 relays: ~25 km²
  500 relays: ~125 km²
```

### 3.2 Mesh topology diagram

```
                     ┌────────────────────────────────────────┐
Leaves  ○ ○ ○ ○ ○ ○  │  Backbone relay mesh (RREQ/OGM/Geo)   │
(50–100)    │         │                                        │
            ▼         │  [R]─────[R]─────────────────[GW]─── Internet
         [Relay]──────┤   \     / \                           │
         [Relay]──────┤   [R]──[R]──[R]──────────────[GW]─── Cellular
         [Relay]──────┤   /         \                         │
○ ○ ○ ○ ○ ○  │        │  [R]─────[R]                         │
            ▼         └────────────────────────────────────────┘
         [Relay]

          Leaf layer         Backbone relay layer     Gateways
         (802.11ah IBSS)     (802.11ah IBSS)         (to internet)
         Leaves wake         Relays always in         Min 2, placed
         every 15 min        receive mode (Opt A)     at opposite edges
```

### 3.3 Natural redundancy from 802.11ah range

At 500 m relay spacing and 500 m radio range, each relay hears 4–8 backbone neighbours. Graph theory defines a network as *k-connected* if it survives removal of any *k-1* nodes without disconnection.

```
K backbone neighbours   k-connectivity   Survives N simultaneous failures
──────────────────────────────────────────────────────────────────────────
2–3                     1-connected      Any 0 failures (fragile)
4–6 (typical)           3-connected      Up to 3 simultaneous relay losses
7–8                     4-connected      Up to 4 simultaneous relay losses

Standard 500 m grid → K=4–6 → 3-connected mesh.
Redundancy is built-in, not engineered deliberately.
```

---

## 4. Hardening the Mesh

### 4.1 Current strengths

```
✅ Natural path redundancy
   K=4–6 backbone peers per relay → 3-connected by default

✅ DTN store-and-forward
   Bundles buffer at relay when path to gateway is lost.
   Data preserved during gateway outage or backbone partition.

✅ Connectivity state machine (ISOLATED/MESH_ONLY/CONNECTED)
   Nodes self-classify and advertise state in HELLO.
   Routing skips ISOLATED next-hops automatically.

✅ Mule custody transfer
   Physically isolated nodes can be serviced by a drone or
   vehicle sweep independently of backbone connectivity.

✅ Bundle store management
   Eviction policy preserves ALERT bundles longest.
   store_pct in HELLO lets mules prioritise fuller relays.

✅ Multi-gateway support
   Anycast routing selects nearest available gateway.
   TIME_SYNC election avoids clock conflicts.
   One gateway failure reroutes to the other automatically.

✅ Alert sensor interrupt
   Critical events bypass the 15-min leaf sleep schedule.
   < 200 ms latency from sensor trigger to gateway (connected).
```

### 4.2 Current weaknesses and mitigations

#### Weakness 1: No minimum backbone peer requirement at installation

**Problem**: The spec allows a relay with only 1 backbone neighbour. If that neighbour fails, the relay and all its leaves become permanently isolated.

**Mitigation**: Add an installation acceptance rule to the provisioning app (Section 3.6 of spec):

```
Relay installation acceptance criteria:
  1. Relay hears ≥ 3 backbone neighbours (from HELLO table)
  2. Relay has a confirmed route to ≥ 1 gateway
     (connectivity_state = CONNECTED, not MESH_ONLY)

Provisioning app checks these conditions before marking
installation as complete. If either fails:
  - App shows map overlay of nearby relays
  - Installer adjusts placement until criteria are met
  - Minimum 3-connectivity enforced across all deployments
```

#### Weakness 2: Leaf peering with only 1 relay

**Problem**: The spec recommends "1–2 nearby relays" for leaf peering but doesn't enforce 2. A leaf peered to only 1 relay has no fallback if that relay fails — the leaf becomes effectively dead until its relay returns.

**Mitigation**:

```
Recommendation: Mandate 2 relay peer links per leaf.

Leaf behaviour change:
  On first wake: attempt PEER_OPEN with 2 closest relays
                 (those with strongest RSSI in HELLO scan)
  On each subsequent wake:
    If only 1 active peer link: scan for a second relay
    If second relay found: establish PEER_OPEN
    If not found: increase LEAF_SCAN_WINDOW_MS to 500 ms
                  to extend scan time for this cycle

Cost: minor — two PEER_OPEN exchanges at first boot only.
      Key resumption means only 1 active session key needed
      per wake after that.

Benefit: leaf survives loss of its primary relay. Falls back
         to secondary relay on next wake automatically.
```

#### Weakness 3: No mule sweep frequency requirement

**Problem**: The spec defines the mule protocol (Section 9.5) but sets no minimum sweep frequency. If the mule sweeps less often than the bundle lifetime (24 hours for sensor data), bundles expire before being collected.

**Mitigation**:

```
Required: mule sweep interval < minimum bundle lifetime

  ALERT bundles:   72-hour lifetime → sweep every 48 hours max
  NORMAL bundles:  24-hour lifetime → sweep every 18 hours max

  Recommended sweep interval: 12 hours (2× daily)
  This provides 2 sweep attempts within the 24-hour window,
  compensating for missed sweeps due to terrain or weather.

  Mule route prioritisation:
    1. Visit relays with highest store_pct first (from HELLO)
    2. Visit previously isolated relays (connectivity = ISOLATED)
    3. Visit all others in geographic sweep order
```

#### Weakness 4: Gateway placement

**Problem**: Two gateways close together don't provide geographic redundancy. A single event (power outage, flooding) can take both out.

**Mitigation**:

```
Minimum gateway placement rules:
  - Minimum 2 gateways per deployment
  - Minimum separation: 20% of deployment diameter
    (for 10 km deployment: ≥ 2 km apart)
  - Place on independent power infrastructure where possible
  - Place at mesh edges, not at the centre
    (a central gateway creates a single high-traffic bottleneck)
  - Each gateway should have ≤ 5 hops to any relay in the mesh

Multi-connectivity backup (optional but recommended):
  GW1: wired ethernet / fibre
  GW2: cellular (different carrier)
  → Deployment survives both ISP outage and fibre cut
```

#### Weakness 5: Relay isolated AND bundle store full

**Problem**: If a relay is ISOLATED (no backbone connectivity) for an extended period, its bundle store fills and NORMAL priority data starts being evicted. Currently no mechanism signals this urgency.

**Mitigation**:

```
Add store_pct threshold alert in HELLO:

  When store_pct ≥ STORE_HIGH_PCT (95%) AND
  connectivity_state = ISOLATED:
    Set ALERT flag in HELLO flags byte
    (reuse existing flags byte, bit 0 reserved → use for this)

  Mule receiving HELLO with this flag:
    Immediately prioritise custody transfer from this relay
    before sweeping others, regardless of planned route order

  This ensures an isolated, full relay is always the mule's
  first stop, preventing data loss from store eviction.
```

---

## 5. Minimum Hardened Deployment Specification

For a production deployment requiring high resilience:

```
Per 100 km² area:
  Relays:           50–100
    Spacing:        500 m (semi-urban), 800 m (open field)
    Min peers:      ≥ 3 backbone neighbours per relay (enforced at install)
    Min routes:     ≥ 1 confirmed gateway route at installation

  Gateways:         2 minimum
    Separation:     ≥ 20% of deployment diameter
    Power:          Independent supply (different circuits or carrier)
    Connectivity:   Different ISP or technology (fibre + cellular)

  Leaves:           50–100 per relay
    Peer links:     2 relays per leaf (mandatory)
    Scan window:    200 ms standard, 500 ms fallback when only 1 peer

  Mule:             1 sweep every 12 hours
    Route order:    Highest store_pct first → ISOLATED relays → sweep order
    Coverage:       Every relay reachable within one sweep

  Monitoring:       Network dashboard polling store_pct from all relays
                    Alert when any relay hits STORE_HIGH_PCT (95%)
                    Alert when any relay stays ISOLATED > 2 × mule interval
```

---

## 6. Installation Verification Checklist

Steps for each relay before moving to the next installation point:

```
☐ 1. Node is powered and LED stops blinking (provisioning complete)
☐ 2. Provisioning app shows connectivity_state = CONNECTED
☐ 3. Relay hears ≥ 3 backbone neighbours in HELLO table
☐ 4. store_pct = 0% (fresh node, no bundles yet)
☐ 5. TIME_SYNC received (time_stale = false)
☐ 6. Node appears on network dashboard within 60 seconds

If step 2 fails: move relay 20–50 m toward nearest backbone relay
If step 3 fails: relay position is a coverage hole — add another relay
If step 5 fails: no gateway in range yet — acceptable, check again after
                 adjacent gateway relay is installed
```

---

## 7. Summary

```
Recommended deployment parameters:

  Relay count:      50–200  (sweet spot for routing and coverage)
  Relay spacing:    300–600 m (terrain dependent)
  Leaves per relay: 50–100   (comfortable, ~14.3 mA relay average ¹)
  Total leaves:     up to 50,000 (500 relays × 100)
  Gateways:         ≥ 2, geographically separated
  Mule sweep:       every 12 hours

Hardening priorities (in order):
  1. Enforce ≥ 3 backbone peers at installation (via provisioning app)
  2. Mandate 2 relay peer links per leaf
  3. Require mule sweep < bundle lifetime (12h sweep, 24h lifetime)
  4. Separate gateways geographically (different power/ISP)
  5. Alert on ISOLATED + STORE_HIGH_PCT combined condition

Natural strengths:
  ✅ 3-connected backbone mesh at standard spacing
  ✅ DTN survives extended gateway outages
  ✅ Mule handles permanently isolated nodes
  ✅ Multi-gateway anycast with automatic failover
  ✅ Alert events bypass 15-min leaf sleep schedule (< 200 ms)
```

---

*Document version: 1.0*  
*Companion spec: rimba-protocol-spec.md Draft 0.26*
