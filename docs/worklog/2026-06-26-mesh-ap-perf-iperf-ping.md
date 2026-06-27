# 2026-06-26 — Mesh vs AP↔STA performance: iperf throughput + ping latency

Characterised the HaLow link throughput (iperf) and latency (ping) across **link type**
(802.11s mesh vs AP↔STA) and **node pair** (Linux↔Linux, Linux↔ESP32, ESP32↔ESP32) on the
1 MHz S1G channel (ch27 / 915.5 MHz, US). Status table lives in
`docs/mesh-ap/rimba-mesh-ap-milestones.md`; this worklog is the standalone record.

## Dedicated performance firmware

So the production apps stay clean (no `IPERF`/`MESH_IPERF` toggles), the perf test runs on
separate firmware: **`firmware/rimba-halow-{mesh,ap,sta}-perf`**. Each is the matching production
app's link bring-up (`rimba-halow-mesh`/`-ap`/`-sta`) plus an **`esp_console` UART REPL** that
registers two console commands:
- **`iperf`** — `espressif/iperf-cmd` (iperf **v2**, port 5001; NOT iperf3-compatible).
- **`ping`** — `esp-qa/ping-cmd`.

Auto-ping / the STA TWT test are disabled in the perf builds; the static IP is still pinned.
`rimba-halow-ap-perf` needs a **custom 2 MB app partition** (`partitions.csv`) — hostapd + the
two cmd components push the binary to ~1.54 MB, over the default ~1 MB / SINGLE_APP_LARGE 1.5 MB
slots (16 MB flash, so room to spare). The production `mesh`/`ap`/`sta` apps were reverted to
clean.

Drive over serial: `iperf -s` one end, `iperf -c <ip>` the other (or `ping <ip> -c N`). Two
gotchas baked into the drivers:
- **Ping first to warm up ARP/HWMP, then iperf.** With no auto-ping, the first TCP connect
  fails `errno 118` (EHOSTUNREACH) until ARP + the mesh path resolve; a ping (or a couple of
  connect retries) primes them.
- **Multi-hop needs all 3 boards reset together** — resetting some while others hold stale peer
  state breaks the forced line (board1↔board2 only via board0 via the peer allowlist).
- Opening a board's USB serial **resets it**, so every ESP measurement pays a ~40–50 s
  boot+peer settle.

## iperf — TCP goodput (Mbit/s)

| | Linux↔Linux | Linux↔ESP32 | ESP32↔ESP32 |
|---|---|---|---|
| **Mesh** (1 hop) | 0.20 | 0.13 | 0.16 |
| **AP↔STA** | 0.79 | 1.10 | 0.84 |
| **Mesh multi-hop** (2 hops) | — | — | ~0.03–0.06 |

L↔L used `iperf3`; ESP cells use the ESP's iperf v2 (so `iperf` v2.2.1 was installed on
chronite/chronium too). ESP↔Linux + Linux-AP↔ESP-STA confirmed at both ends (server saw the
client's bytes). The 5 Mbit/s default UDP offered rate overran the link (70% loss); 500 kbit/s
was clean — keep offered UDP under the ceiling. PHY rate was 72 Mbit/s VHT-MCS7 at −21 dBm, but
real goodput is sub-Mbps — **1 MHz channel airtime is the cap, not the radio**.

## ping — RTT (median)

| | Linux↔Linux | Linux↔ESP32 | ESP32↔ESP32 |
|---|---|---|---|
| **Mesh** (1 hop) | ~25 ms | ~11–20 ms | ~18 ms (0% loss) |
| **AP↔STA** | ~14 ms | ~10–15 ms | ~15 ms (0% loss) |
| **Mesh multi-hop** (2 hops) | — | — | ~50 ms median (35–59 ms, occasional ~850 ms spike, ~8% loss) |

## Findings

1. **AP↔STA single-hop (~0.8–1.1 Mbps) is ~5× the mesh single-hop (~0.13–0.20 Mbps).** The
   mesh's 4-address header + Mesh Control + HWMP cost real airtime on a 1 MHz channel; the
   AP↔STA path is leaner.
2. **The second mesh hop roughly quarters throughput** (0.16 → ~0.04) **and doubles RTT**
   (~18 → ~50 ms), with added loss/spikes — the relay store-and-forwards on the same shared
   channel, and the 30 s path lifetime triggers re-discovery stalls under load.
3. **Within a row, node type barely matters** (Linux vs ESP) — channel airtime dominates, not
   the host stack. Latency tracks **hop count, not link type**.
4. Everything is **sub-Mbps and ~15–50 ms** on this 1 MHz link — sets realistic expectations for
   the Mesh-gate vs a flat AP↔STA topology.
