# Rimba Protocol — Routing Algorithm Alternatives and Comparison

**Companion to**: rimba-protocol-spec.md Draft 0.26  
**Document version**: 1.0

---

## 1. Overview

Rimba uses three routing mechanisms (Section 8.0 of spec):

- **Mode 1**: OGM proactive (B.A.T.M.A.N.-inspired, small networks)
- **Mode 2**: RREQ/RREP reactive (AODV-inspired, large networks)
- **Mode 3**: Geographic greedy (GPSR-inspired, GPS-equipped relays)
- **Base layer**: DTN store-and-forward (BPv7)

This document compares each against its principal alternatives to explain why the current design was chosen and where alternatives might be superior.

---

## 2. Proactive Routing Alternatives

Proactive protocols maintain forwarding tables continuously. Routes are always available — zero discovery latency. Cost: ongoing overhead traffic even when no data is moving.

### 2.1 OLSR (Optimized Link State Routing) vs Rimba OGM

**OLSR (RFC 3626 / RFC 7181):**

OLSR reduces proactive flooding overhead by electing a **Multipoint Relay (MPR)** set: the minimum subset of 1-hop neighbours that collectively reach all 2-hop neighbours. Only MPR nodes re-flood Topology Control (TC) messages. Non-MPR nodes send HELLO to build neighbour knowledge but do not re-flood TC.

```
Pure flood:  Every node re-floods every message → O(N²) traffic
OLSR MPR:    Only elected MPRs re-flood TC → O(MPR_count × N)
             Typically 20–40% of nodes are MPRs in sparse mesh
Rimba OGM:   Every node re-floods OGMs but Trickle suppresses duplicates
             → effective overhead comparable to OLSR in stable networks
```

**Empirical comparison (from testbed research):**

OLSR gives a better average value of throughput compared to BATMAN, while BATMAN routing protocol gives better Round Trip Delay. In IoT environments, BATMAN recovers at least 73.9% faster than OLSR after a topology disruption, because BATMAN only determines whether to switch a route by comparing the number of OGM packets received from a different next-hop.

A 2024 comparative evaluation of OLSR, BATMAN, and Babel using 11 Raspberry Pi 4 nodes showed that OLSR achieved the highest outdoor bandwidth and most stable jitter, while BATMAN had the best packet delivery ratio.

| Dimension | OLSR | Rimba OGM (BATMAN-style) |
|---|---|---|
| Flooding reduction | MPR set (mathematical minimum) | Trickle timer (empirical suppression) |
| Overhead | Lower (MPR-only TC) | Slightly higher (all nodes OGM) |
| Recovery speed | Slower (MPR recalculation) | Faster (no MPR concept) |
| Route stability | Moderate | Better (OGM count metric) |
| Complexity | High (MPR selection algorithm) | Lower |
| DTN support | No | No (OGM mode is for connected operation) |
| MCU suitability | Needs 2-hop topology table | Simple accumulation |

**Verdict**: OLSR achieves marginally better throughput via mathematically optimal MPR selection. Rimba's OGM recovers faster from topology changes and is simpler to implement on a constrained MCU. For a sensor mesh where resilience matters more than peak throughput, OGM is the better fit. If overhead reduction becomes critical at very large scale, OLSR's MPR mechanism could replace OGM as a future enhancement.

---

### 2.2 Babel vs Rimba OGM

Babel (RFC 8966) is a proactive distance-vector protocol with provably loop-free convergence using a "feasibility condition" that prevents routing loops without sequence numbers.

Babel initially provided the highest indoor bandwidth but declined significantly with increased network size.

| Dimension | Babel | Rimba OGM |
|---|---|---|
| Loop prevention | Feasibility condition (no seq#) | OGM sequence numbers |
| Convergence | Fast (triggered updates) | Moderate (OGM interval) |
| Scalability | Degrades at large N | Trickle-limited, stable |
| Complexity | Moderate | Low |
| MCU libraries | Contiki port exists | Must implement from scratch |

**Verdict**: Babel is more theoretically rigorous but degrades at scale. Not a significant advantage over Rimba OGM for the target deployment size.

---

### 2.3 RPL (Routing Protocol for Low-Power Lossy Networks) vs Rimba

RPL (RFC 6550) is the IETF standard routing protocol for IoT on IEEE 802.15.4 / 6LoWPAN. It builds a Directed Acyclic Graph (DODAG) rooted at a gateway/border router.

RPL is designed specifically for static networks that do not involve mobility or topological changes. RPL guarantees continuous connectivity between nodes and mitigates the risk of data loss in stationary IoT applications that do not involve mobility or alterations in network configuration.

RPL routing protocol offers adaptation to changing network topology and its performances are superior to AODV and DYMO in 6LoWPAN, with higher stability.

```
RPL topology:
       [Gateway/Root]
       /    |    \
    [R1]  [R2]  [R3]     ← preferred parents
    / \    |     |
  [L] [L] [L]  [L]       ← leaves
  
Upward traffic (sensor → gateway): optimal, always follows DODAG
Downward traffic (gateway → sensor): requires stored-down routes
                                      (optional, often not implemented)
Peer-to-peer (sensor → sensor): poor support, route through root
```

| Dimension | RPL | Rimba |
|---|---|---|
| Topology | Tree (DODAG) | Mesh (any-to-any) |
| Radio | 802.15.4 (250 kbps) | 802.11ah (150 kbps usable) |
| Upward traffic | Excellent | Good |
| Downward traffic | Poor (optional feature) | Good (DTN + relay buffering) |
| Peer-to-peer | Poor (routes through root) | Full mesh routing |
| DTN support | None | First-class (BPv7) |
| Trickle timer | Yes (for DIO messages) | Yes (for HELLO/OGM) |
| IETF standard | Yes (RFC 6550) | No (custom) |
| MCU implementations | Contiki-NG, RIOT OS | Must implement from scratch |

**Verdict**: RPL is well-suited for simple sensor collection trees where data flows primarily upward to a single gateway and all nodes are always connected. It is the wrong fit for Rimba because: (1) Rimba targets mesh-redundant topologies, not trees; (2) Rimba needs DTN for disconnected operation; (3) RPL on 802.15.4 is designed for ~250 kbps, not 802.11ah's 150 kbps usable; (4) downlink commands to leaves are important for Rimba.

RPL is worth considering if a future Rimba deployment uses 802.15.4 radios instead of HaLow.

---

## 3. Reactive Routing Alternatives

Reactive protocols discover routes only when needed. No overhead when idle. Route discovery latency on first packet.

### 3.1 DSR (Dynamic Source Routing) vs Rimba RREQ/RREP

DSR (RFC 4728) uses **source routing**: the source node includes the complete hop-by-hop path in each packet header. Intermediate nodes do not maintain routing tables — they simply forward based on the header.

```
RREQ flood builds routes at every intermediate node:
  A → B → C → D (dest)
  B records: "to reach D, next hop is C"
  C records: "to reach D, next hop is D"
  All intermediate nodes have routes cached

DSR: source includes full path in every packet:
  Packet header: [A → B → C → D]
  B just strips its entry and forwards: [A → C → D]
  No routing tables needed at B or C

Discovery: RREQ accumulates the path in its header as it floods
```

| Dimension | DSR | Rimba RREQ/RREP |
|---|---|---|
| Intermediate node tables | Not needed | Required |
| Packet header overhead | Grows with path length | Fixed (6 bytes dst in mesh header) |
| Route caching | At source only | At every relay on path |
| Header at 10 hops (6-byte IDs) | +60 bytes per packet | 0 extra |
| Route discovery | Same flood as RREQ | RREQ + RREP |
| Asymmetric links | Handles naturally | Requires bidirectional path |
| MCU memory | Lower (no routing table) | Higher (routing table) |

**Header overhead example for Rimba's sensor data:**

```
Typical sensor bundle: 300 bytes payload
DSR at 5 hops (6-byte node IDs): 5 × 6 = 30 bytes = +10% overhead
DSR at 10 hops:                  10 × 6 = 60 bytes = +20% overhead
Rimba RREQ/RREP: 0 extra bytes per hop in data packets
```

**Verdict**: DSR saves routing table memory at intermediate nodes but adds header overhead to every packet. For Rimba's 300-byte sensor bundles over 3–5 hops, the overhead is acceptable (~6-10%). The bigger advantage is that intermediate relays don't need routing tables — valuable for very memory-constrained nodes. However, Rimba already targets ESP32-S3 with PSRAM, so memory is not the bottleneck. RREQ/RREP is simpler to implement correctly and generates no per-packet overhead. DSR becomes more attractive if Rimba is ported to severely constrained MCUs (< 64 KB RAM).

---

### 3.2 TORA (Temporally Ordered Routing Algorithm) vs Rimba RREQ/RREP

TORA builds directed acyclic graphs using a "height" metric and multiple routes to a destination. Highly resilient to topology changes but complex.

| Dimension | TORA | Rimba RREQ/RREP |
|---|---|---|
| Multiple routes | Yes (DAG provides alternatives) | No (single best route) |
| Topology recovery | Very fast (local repair) | Moderate (RERR + new RREQ) |
| Complexity | Very high | Moderate |
| MCU suitability | Poor (complex state machine) | Good |

**Verdict**: TORA's multi-route DAG approach is powerful but the state machine complexity makes it unsuitable for ESP32-S3 implementation alongside the rest of the Rimba stack. Not recommended.

---

## 4. Hybrid Routing Alternatives

### 4.1 ZRP (Zone Routing Protocol) vs Rimba Dynamic Switching

ZRP divides the network into zones based on hop radius. Within a zone: proactive routing. Between zones: reactive. Zone radius is a fixed parameter set at provisioning.

```
ZRP with zone radius = 2:
  Node A knows routes to all nodes within 2 hops proactively.
  For nodes beyond 2 hops: reactive RREQ/RREP.

Rimba dynamic switching:
  Switches based on ogm_originator_count (observed network size).
  In small networks: all proactive (OGM).
  In large networks: all reactive (RREQ/RREP).
  Automatically adapts — no fixed zone radius.
```

| Dimension | ZRP | Rimba Dynamic Switching |
|---|---|---|
| Zone definition | Fixed hop radius (configured) | Dynamic (originator count) |
| Adaptation | Manual zone radius tuning | Automatic, no config needed |
| Within-zone | Proactive routing table | OGM forwarding table |
| Between-zone | Reactive RREQ/RREP | RREQ/RREP |
| Overhead | Between proactive and reactive | Same as pure reactive |
| Density adaptation | None | Uses originator count threshold |

**Verdict**: ZRP is closest in philosophy to Rimba's dynamic switching, but requires manual zone radius tuning. Rimba's approach is fully automatic, using the originator count to decide globally whether to run proactive or reactive. This is simpler to deploy and operates correctly across different deployment sizes without parameter tuning.

---

### 4.2 HWMP (802.11s Hybrid Wireless Mesh Protocol) vs Rimba

HWMP is the mandatory path selection protocol for 802.11s. It combines a proactive tree (using Root Announcement, RANN) with reactive per-destination discovery (PREQ/PREP).

| Dimension | HWMP | Rimba |
|---|---|---|
| Standard | IEEE 802.11s | Custom |
| Proactive component | RANN (root-centred tree) | OGM (decentralised) |
| Reactive component | PREQ/PREP | RREQ/RREP |
| DTN support | None | Yes (BPv7) |
| Sleep scheduling | None (802.11s is always-on) | TDMA leaf scheduling |
| Metric | Airtime link metric (ALM) | Hop count + link quality |
| Requires | Linux (mac80211) | MCU (ESP32-S3) |

**Verdict**: HWMP is not available on the ESP32-S3 + MM6108 IBSS stack (it requires 802.11s and Linux mac80211). Even if ported, it lacks DTN and sleep scheduling. Not applicable.

---

## 5. DTN Routing Alternatives

Rimba uses simple **ALERT-first, oldest-first** queuing with mule custody transfer. Several DTN routing algorithms offer smarter forwarding decisions.

### 5.1 Epidemic Routing vs Rimba DTN

Epidemic routing replicates every bundle to every encountered node. Maximum delivery probability; minimum delivery delay. Also maximum resource consumption.

```
Every time two nodes meet:
  Exchange all bundles the other doesn't have
  Both nodes now carry all bundles
  Delivery probability: 1.0 (eventually)
  Store usage: ~N × bundle_size (N = bundle count in network)
```

| Dimension | Epidemic | Rimba DTN |
|---|---|---|
| Delivery probability | Maximum | High (mule covers all relays) |
| Storage per relay | Very high (all bundles) | Low (local bundles only) |
| Network traffic | Very high (full exchange) | Low (custody transfer only) |
| Intelligence | None | None (oldest-first) |
| Mule integration | N/A | First-class (custody protocol) |

**Verdict**: Epidemic routing is wildly inappropriate for constrained sensor nodes with 256 KB bundle stores. Rimba's DTN correctly limits bundle propagation to the routing path plus mule custody.

---

### 5.2 PRoPHET (Probabilistic Routing) vs Rimba DTN

PRoPHET tracks a **delivery predictability** metric P(A,B) between nodes: the probability that node A will encounter node B in the future, based on historical contact frequency. Bundles are forwarded to nodes with higher P toward the destination.

```
P(A,B) updated on every encounter between A and B:
  P(A,B) = P(A,B)_old + (1 − P(A,B)_old) × P_init

When A meets C (relay decision):
  If P(C, destination) > P(A, destination):
    Forward bundle to C
    C is "more likely" to reach destination
```

| Dimension | PRoPHET | Rimba DTN |
|---|---|---|
| Routing intelligence | History-based probability | None (oldest-first) |
| Memory per node | O(N) predictability entries | O(1) per bundle |
| Suited for | Mobile opportunistic | Static mesh + planned mule |
| Mule integration | Mule as high-P node | Explicit custody protocol |
| Implementation | Moderate | Simple |

**Verdict**: PRoPHET makes most sense when nodes are mobile and encounters are random. In Rimba's static mesh, relay encounter patterns are perfectly predictable (they're stationary). The mule's planned route makes PRoPHET's probabilistic tracking unnecessary. Simple oldest-first is sufficient. If the mule ever becomes autonomous with an unpredictable route, PRoPHET would become more relevant.

---

### 5.3 Spray and Wait vs Rimba DTN

Spray and Wait distributes L copies of a bundle to the first L distinct relay nodes encountered, then waits for direct delivery.

```
Binary Spray and Wait (L=8):
  Source sprays 8 copies to 8 different relays
  Each relay carries 1 copy
  One of the 8 copies directly encounters the destination
  
Rimba: 1 copy per bundle, held until route or mule
```

| Dimension | Spray and Wait | Rimba DTN |
|---|---|---|
| Copies per bundle | L (e.g. 8) | 1 |
| Storage per relay | 1/L of epidemic | 1× (local only) |
| Best for | Mobile opportunistic | Static + planned mule |
| Delivery latency | Lower than wait-only | Depends on mule schedule |

**Verdict**: Spray and Wait is designed for mobile opportunistic networks where you want bounded resource usage with better delivery than pure "wait." Rimba's relays are static and the delivery path is the backbone or planned mule sweep — there's no benefit to spraying copies across static relays.

---

### 5.4 CGR (Contact Graph Routing) vs Rimba DTN

CGR, used in NASA's ION-DTN for deep space networks, builds a **contact graph** from predicted contact schedules: "Node A will be in contact with Node B from T1 to T2." CGR then computes the earliest-delivery path through this graph.

```
Contact schedule example:
  Mule M contacts Relay R7 daily at 14:00–14:10
  Mule M contacts Gateway G daily at 15:00–15:05
  
CGR computation:
  "To deliver bundle to Gateway by today 15:30:
   Hold until 14:00, transfer to Mule at R7,
   Mule carries to Gateway by 15:05"
```

| Dimension | CGR | Rimba DTN |
|---|---|---|
| Contact prediction | Required (published schedule) | Not required |
| Delivery optimality | Optimal for known schedules | Oldest-first heuristic |
| Mule integration | First-class (scheduled contacts) | Custody transfer on encounter |
| Complexity | High | Low |
| Implementation | ION-DTN (portable to MCU) | Custom simple queue |

**Verdict**: CGR is more powerful than Rimba's DTN for well-scheduled mule routes. If a Rimba deployment has a drone mule that follows a fixed daily schedule, CGR would allow the relay to hold bundles and deliver them exactly when the mule arrives, rather than just queuing oldest-first. This is a meaningful future enhancement for deployments with predictable mule schedules.

---

## 6. Geographic Routing Alternatives

### 6.1 Full GPSR (Greedy Perimeter Stateless Routing) vs Rimba Geographic

Rimba implements GPSR-style greedy forwarding but replaces the perimeter routing void handler with DTN buffering. Full GPSR uses **right-hand rule perimeter routing** to navigate voids:

```
Full GPSR void handling:
  Void detected at node A (no closer neighbour to destination)
  Switch to perimeter mode: traverse perimeter of the void
  using right-hand rule (always take the rightmost edge)
  Resume greedy when a node closer to destination is found

Rimba void handling:
  Void detected → DTN buffer
  Wait for topology change or mule
```

Full GPSR requires a **planarized graph** (Gabriel Graph or Relative Neighbourhood Graph) for correct perimeter routing. Planarization removes edges that would cause the right-hand rule to cycle. This requires knowledge of all neighbours' positions and additional computation.

| Dimension | Full GPSR | Rimba Geographic |
|---|---|---|
| Void handling | Perimeter routing (right-hand rule) | DTN buffer |
| Requires | Planarized graph | Nothing extra |
| Void delivery latency | Low (immediate alternative path) | High (waits for mule/route) |
| Complexity | High | Low |
| Best for | Connected mobile networks | Static sparse mesh with DTN |
| Memory | Position table + planar graph | Position table only |

**Verdict**: Full GPSR void handling is better when you cannot afford to buffer (real-time applications, no DTN layer). For Rimba's sensor mesh where bundles can wait and mules eventually collect data, DTN void handling is simpler and equally effective. If Rimba were used for real-time alert systems where DTN latency is unacceptable, full GPSR perimeter routing would be worth adding.

---

### 6.2 GEAR (Geographic and Energy Aware Routing) vs Rimba Geographic

GEAR extends geographic greedy routing by incorporating remaining energy into the forwarding metric, balancing load across the network to avoid draining individual nodes.

```
GEAR score = distance_to_destination × (1 / remaining_energy)
             × (256 / link_quality)

Higher remaining energy → lower score → preferred forwarder
```

| Dimension | GEAR | Rimba Geographic |
|---|---|---|
| Energy awareness | Yes (energy in metric) | No |
| Load balancing | Implicit (high-energy preferred) | None |
| Battery reporting | Required in HELLO | Not currently in HELLO |
| Implementation | Slightly more complex | Simpler |
| Suitable for | Battery-powered relay mesh | Battery-powered relay mesh |

**Verdict**: GEAR's energy term is directly applicable to Rimba. Relay battery level is not currently advertised in HELLO, but adding it would allow GEAR-style forwarding that avoids routing through low-battery relays. This is worth adding as a future enhancement when battery monitoring is implemented. The implementation delta from Rimba's current geographic routing is small.

---

## 7. Summary Comparison Table

| Protocol | Category | vs Rimba equivalent | Better than Rimba when | Worse than Rimba when |
|---|---|---|---|---|
| **OLSR** | Proactive | vs OGM | Very large proactive network (lower overhead via MPR) | MCU resource-constrained; topology changes frequently |
| **Babel** | Proactive | vs OGM | Loop-free convergence needed without sequence numbers | Large networks (overhead grows) |
| **RPL** | Proactive IoT | vs OGM + RREQ | 802.15.4 radio; pure upward collection tree | 802.11ah radio; DTN needed; peer-to-peer required |
| **DSR** | Reactive | vs RREQ/RREP | Intermediate nodes severely memory-constrained | Long paths (header bloat); PSRAM available |
| **TORA** | Reactive | vs RREQ/RREP | Multi-path resilience critical | MCU implementation (too complex) |
| **ZRP** | Hybrid | vs dynamic switching | Zone radius is known and stable | Adaptive deployment; dynamic network size |
| **HWMP** | Hybrid | vs OGM + RREQ | Standard 802.11s required | MCU environment; DTN needed |
| **Epidemic** | DTN | vs oldest-first | Delivery probability must be maximised regardless of cost | Any resource-constrained node |
| **PRoPHET** | DTN | vs oldest-first | Mobile nodes, random encounters | Static mesh with predictable mule |
| **Spray+Wait** | DTN | vs oldest-first | Mobile nodes, bounded resource use | Static relays, planned mule |
| **CGR** | DTN | vs oldest-first | Fixed mule schedule known in advance | Unpredictable or absent mule |
| **Full GPSR** | Geographic | vs greedy+DTN | Connected network; no DTN; real-time delivery required | Sparse mesh; DTN available; static deployment |
| **GEAR** | Geographic | vs greedy+DTN | Energy-balanced routing needed | Battery levels not monitored |

---

## 8. Recommendations for Rimba

### Current design is well-chosen for the target use case

The combination of OGM + RREQ/RREP + geographic overlay + DTN covers the full deployment spectrum from a 5-node test bench to a 1,000-relay sensor mesh with isolated nodes and mule delivery. No single alternative covers this range as cleanly.

### Specific future enhancements worth tracking

```
1. GEAR energy term (small delta from current geographic routing)
   Add remaining_battery_pct to HELLO fields
   Modify geographic score: score × (256 / battery_pct)
   Avoids routing through low-battery relays → improves network longevity

2. CGR for planned mule routes (moderate implementation effort)
   If mule adopts a fixed published schedule, CGR would allow
   relays to time bundle handoff exactly, reducing mule contact
   window duration and improving custody efficiency.
   ION-DTN's CGR implementation is portable to ESP32-S3.

3. OLSR MPR as replacement for OGM at very large scale (> 1000 nodes)
   If network grows beyond RREQ/RREP reactive mode's practical limit,
   MPR-based proactive routing within geographic zones would allow
   larger proactive networks. Complexity is significant.

4. Full GPSR perimeter routing (large implementation effort)
   Only warranted if Rimba is used for real-time applications
   where DTN buffer latency is unacceptable (e.g., safety alerts).
   Currently: alert bundles bypass DTN via sensor GPIO interrupt.
   For most use cases, DTN void handling is sufficient.
```

### What not to change

```
RPL:         Wrong topology model (tree vs mesh) for Rimba's
             redundancy requirements.

Spray+Wait:  Static relays make multi-copy spreading wasteful.

Epidemic:    Completely incompatible with constrained storage.

DSR:         PSRAM available, header overhead not worth the
             routing table memory savings.
```

---

*Document version: 1.0*  
*Companion spec: rimba-protocol-spec.md Draft 0.26*
