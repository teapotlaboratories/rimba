# Mesh-gate client ceiling — 4 to 16, and the four things that had to move with it

**Date:** 2026-08-02
**Task:** the tracked *"raise the gate's client ceiling to ~16-32"* item.
**Outcome:** raised to **16**, not 32, with the arithmetic that decides it. Three apps also had to be
moved off the default partition table before anything could be added to them at all.

---

## Two corrections before any work

**1. The binding ceiling was 4, not 8.** I had "gate ceiling is 8 clients" in the backlog and repeated it
several times. In *this* repo the gate admits **4**: `LINK_MAX_STAS 4`
(`firmware/rimba-halow-mesh-ap/main/app_main.c:62`, applied at `:683 ap_args.max_stas`), enforced in
morselib at `umac_ap.c:742-747` which refuses any AID ≥ `max_stas` with `MMWLAN_NO_MEM`. The 8 I was
carrying is `GATE_MAX_CLIENTS`, the L2-bridge table — already 2× the admission limit, so it never binds.
(The 8 in my memory came from the *examples* repo; this repo is stricter.)

**2. Two apps were within 5 KB of not fitting**, found by building before editing:

| app | free on the default `0x177000` partition |
|---|---|
| `rimba-halow-mesh-ap` | **4,640 B (0 %)** |
| `test-mesh-gate-ap` | **5,744 B (0 %)** |
| `rimba-halow-ap` | 15,776 B (1 %) |
| `test-mesh-gate`, `rimba-halow-mesh` | ~33 KB (2 %) |

**Nothing could be added to the two gate apps at all.** That is a repo-wide tripwire, not something
specific to this task — any feature work on the shipped gate would have failed the build. All three now
carry their own 2 MB `partitions.csv` (16 MB flash; precedent `rimba-halow-ap-perf`), taking them to
**27-28 % free**.

> ⚠ `sdkconfig` is cached per build directory, so adding `CONFIG_PARTITION_TABLE_CUSTOM` to
> `sdkconfig.defaults` does **nothing** until `rm -rf build/<app>/<board>`. The build cheerfully reports
> the *old* partition size, which reads as the override having failed.

---

## Why 16 and not 32

The gate runs a **proactive proxy-ARP push**: for every (AP client, mesh host) pair it sends one reliable
unicast, every 3 s — `gw_arp_announce_once()`, a nested scan over the shared ARP table. It is
O(n_ap × n_mesh) in **both** directions.

At ~4.85 ms per push at 1 MHz MCS0 with 5 mesh nodes:

| clients | pushes/period | at 3 s | at 15 s |
|---|---|---|---|
| 8 | 80 | 12.9 % | 2.6 % |
| **16** | **160** | 25.8 % | **5.2 %** |
| 32 | 320 | **51.7 %** | 10.3 % |

At 32 clients on the old period the gate would spend **more than half its airtime teaching ARP**. So the
period moves 3 s → **15 s** (entries live `GW_ARP_TTL_MS` = 5 min, so each mapping still refreshes 20×
per lifetime) and the ceiling stops at 16.

**A harder ceiling that would have bitten silently:** `GW_ARP_MAX = 24` holds **both** sides — AP clients
*and* learned mesh hosts. With 5 mesh nodes that saturates at ~19 clients, after which the LRU thrashes
and proxy-ARP quietly stops resolving rather than failing. Raised to **48**.

---

## The changes

| what | from | to | why |
|---|---|---|---|
| `LINK_MAX_STAS` | 4 | **16** | the admission cap; the thing that actually bound |
| `GATE_MAX_CLIENTS` | 8 | `LINK_MAX_STAS` | see below |
| `GW_ARP_MAX` | 24 | 48 | holds both sides; was saturating at ~19 clients |
| ARP push period | 3 s | 15 s | 25.8 % → 5.2 % airtime at the new ceiling |
| `CONFIG_HALOW_AP_MAX_STAS` | 20 (default) | 16 | morselib's own cap |
| `CONFIG_LWIP_DHCPS_MAX_STATION_NUM` | 8 (IDF default) | 16 | see below |

Applied to `rimba-halow-mesh-ap` (the shipped gate) and `test-mesh-gate-ap` (its T2 mirror), which are
line-for-line twins — leaving them divergent would mean the regression tier stopped exercising the
shipped configuration.

### Two silent-failure classes closed

**`GATE_MAX_CLIENTS` is now derived, not an independent number.** It was `8` while `LINK_MAX_STAS` was
`4`. Had anyone raised the admission cap without noticing the table, a client past the table's end would
associate, get a lease, transmit into the mesh — and **silently never receive a mesh reply**, because
`gate_is_ap_client()` would not recognise it. Now `#define GATE_MAX_CLIENTS LINK_MAX_STAS` with a
`_Static_assert`, so the two cannot disagree.

**The DHCP pool would have silently under-served.** `CONFIG_LWIP_DHCPS_MAX_STATION_NUM` defaults to **8**
and was never overridden. A gate admitting 16 stations would have addressed the first 8 and left the rest
associated-but-unaddressed.

---

## A comment that was actively wrong, and is now corrected

The gate's netif setup claimed:

> *"The dhcps pool starts at 10.9.9.2 and spans `CONFIG_LWIP_DHCPS_MAX_STATION_NUM` leases (default 8 →
> .2-.9), so at the default it stays clear of the mesh nodes' 10.9.9.100-163 static range"*

**`MAX_STATION_NUM` caps the lease-record list, not the address range.** The range is derived
independently: start = server + 1 = `.2`, end = start + `DHCPS_MAX_LEASE`, and `DHCPS_MAX_LEASE` is
`0x64` = 100 (`dhcpserver.h:66`). So the pool is **`.2`-`.101` regardless of that Kconfig**, and its top
two addresses **do** overlap the mesh nodes' self-assigned `.100`-`.163`.

The wrong model was doubly misleading: it made raising the ceiling look dangerous when it isn't, and it
made the real overlap look impossible.

**Recorded, not fixed.** Reaching `.100` needs on the order of a hundred concurrent clients, so it is
unreachable at this ceiling. Pinning the range means calling `esp_netif_dhcps_option()` before the netif's
AUTOUP auto-starts dhcps, and getting that ordering wrong **boot-loops the gate** (the `DHCP_NOT_STOPPED`
path the surrounding comment already warns about). That is a real risk taken for an unreachable bug, on a
change that could not be bench-verified at ~100 clients anyway.

---

## Ceilings deliberately left alone

Recorded in the code next to `LINK_MAX_STAS` so the next person raising it sees them:

- **`MESH_MPP_MAX = 32`** (`umac_mesh.c:2096`, halow submodule) maps off-mesh host → proxying mesh node
  and lives on **every** mesh node. Each AP client behind each gate consumes an entry on every peer, so 16
  clients on one gate already fills half of it mesh-wide. **This is the ceiling that really caps a
  multi-gate mesh** — at 32 clients a second gate would evict, and a lost `mpp` entry silently breaks the
  mesh→client return leg. It independently confirms 16 as the right stopping point.
- **`MMPKTMEM_TX_POOL_N_BLOCKS = 32`** caps in-flight TX, and the gate **drops silently** on allocation
  failure (`gw_tx_ap_frame` returns on a NULL pkt).
- The flat `/24` tops out near 98 usable addresses.

---

## Verification

| check | result |
|---|---|
| All three apps build | **clean**, 27-28 % partition free (was 0-1 %) |
| Gate boots with raised ceilings | **clean** — mesh vif up, RANN on, AP vif up concurrent, `10.9.9.1/24` + DHCP, L2 bridge ready, no asserts |
| Client associates | **PASS** — `wpa_state=COMPLETED`, BSSID `6a:24:99:44:6b:b7` |
| DHCP from the raised pool | **PASS** — leased `10.9.9.2`, 7200 s |
| Traffic across the bridge | **PASS** — 4/4 ping, 7.1 / 22.5 / 67.1 ms |

Bench left radio-silent: board0 on `rimba-hello` with zero HaLow lines, chronite's supplicant killed and
`wlan1` down.

> ⚠ **CAPACITY AT 16 IS NOT PROVEN, and cannot be on this bench.** Three ESP boards and a few Linux nodes
> cannot make 16 clients. What is verified is that the constants are consistent, the RAM fits (~14.6 KB
> of `umac_sta_data` at 16), the apps build with room, and the gate does not regress at the client count
> available. **This is a structural claim** — exactly the kind this project criticised `ap_scale`'s "255"
> for being, and it should not be quoted as measured capacity. The code says so where the ceiling is
> defined.

## A diagnosis worth recording

The first bridge test reported no address and 100 % ping loss, which reads as a regression. It was not:
**`dhclient` is not installed on chronite** (`dhcpcd` is), so the DHCP step never ran and the ping came
from an interface with no address. Same shape as the documented "a source with no IP fails ping with an
empty mpath — looks like a forwarding bug" trap. Checking which tools exist before believing the failure
took one command.

## What this does and does not unblock

It makes the gate usable beyond 4 clients, which is worth having on its own.

It does **not** unblock the RAW break-even measurement, and I should correct my own earlier framing: I
wrote that raising the ceiling was step 1 toward observing RAW's benefit. Raising a ceiling does not
create clients — that measurement still needs many-STA hardware the bench does not have. Necessary,
nowhere near sufficient.
