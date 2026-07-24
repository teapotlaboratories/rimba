# rimba-halow-mesh-ap — the mesh-gate (mesh + AP L2-bridged on one radio)

Runs an 802.11s HaLow **mesh** and a co-channel **SoftAP** concurrently on a
single MM6108, and **bridges** them at L2 onto ONE flat subnet — the all-ESP32
"mesh-gate". Stations that associate to the SoftAP share the mesh nodes'
`10.9.9.0/24` and reach any of them directly; the gate proxies each side's
frames across the 802.11s Address-Extension bridge (unicast + broadcast, both
ways) and answers ARP across it (proxy-ARP). The mesh runs on the primary vif,
the AP on a secondary vif; each gets its own `esp_netif`, RX is demuxed per vif,
and TX is tagged with the originating vif.

One flat subnet:

- **mesh** `10.9.9.<gw>/24` — primary vif; the host part is derived from the
  board's MAC, printed at boot as `mesh netif static IP 10.9.9.x (L2-bridge gate host)`.
- **AP** `10.9.9.1/24` — secondary vif, with a **DHCP server** that hands AP
  clients a `10.9.9.x` (a small pool from `10.9.9.2`, clear of the mesh nodes'
  `10.9.9.100–163`). So an AP client is zero-config: it leases an address and
  reaches any mesh node directly — no static IP, no route, no IP forwarding.

Needs the `components/halow` mesh + AP concurrency + 802.11s Address-Extension
support. (No `CONFIG_LWIP_IP_FORWARD` — the gate is a pure L2 bridge.)

## Build + flash

Run from the repo root. Pick the `PORT` your board enumerates on
(`/dev/ttyACM*`):

```bash
make build APP=rimba-halow-mesh-ap BOARD=proto1-fgh100m
make flash APP=rimba-halow-mesh-ap BOARD=proto1-fgh100m PORT=/dev/ttyACM0
make monitor APP=rimba-halow-mesh-ap PORT=/dev/ttyACM0     # serial console, Ctrl-] to quit
```

## Pairing it up

The gate is only interesting with something on each side. A full three-board
demo — a zero-config STA under the AP pinging a node on the mesh:

```bash
# board 0 — this gate (mesh 10.9.9.<gw> + AP 10.9.9.1 + DHCP)
make flash APP=rimba-halow-mesh-ap BOARD=proto1-fgh100m PORT=/dev/ttyACM0

# board 1 — a mesh peer on the same Mesh ID + channel (static 10.9.9.<100+lowbits>)
make flash APP=rimba-halow-mesh    BOARD=proto1-fgh100m PORT=/dev/ttyACM1

# board 2 — a station that joins the AP: DHCPs a 10.9.9.x + pings the mesh node
make flash APP=rimba-halow-sta     BOARD=proto1-fgh100m PORT=/dev/ttyACM2
```

Nothing else to configure: the STA is a DHCP client by default and the mesh
nodes are on the same flat subnet, so the STA's `10.9.9.x` reaches a mesh node's
`10.9.9.x` directly — the gate bridges the traffic and proxy-ARP resolves it.
Watch the gate's console for the STA authorizing (`AP client joined: …`), the
`L2 bridge ready` line, and the `S5b`/`S5c`/`proxy-ARP push` bridge activity; on
the STA, `DHCP lease 10.9.9.x` then `reply from 10.9.9.100`.

## What to change for your own network

Edit the `#define`s at the top of [`main/app_main.c`](main/app_main.c):

- **`MESH_ID`** / **`MESH_S1G_CHAN`** — the mesh network name and S1G channel
  (defaults `"rimba-mesh"`, `27`). Every mesh node must share both. Change for
  your network.
- **`LINK_SSID`** / **`LINK_PSK`** — the SoftAP name and passphrase (>= 8 chars,
  required for SAE). Keep these in sync with the `rimba-halow-sta` client.
- **`LINK_S1G_CHAN`** / **`LINK_OP_CLASS`** — the AP's S1G channel and global
  operating class (defaults to US 915.5 MHz, 1 MHz BW). Match your regulatory
  domain; on a single radio the AP and mesh share the channel.
- **`AP_SUBNET_IP`** / **`AP_SUBNET_MASK`** — the gate's AP-side host on the flat
  subnet (default `10.9.9.1/24`); the DHCP pool derives from it. Keep it on the
  same subnet as the mesh (`mesh_net_task`'s `10.9.9.` prefix), clear of the mesh
  nodes' `10.9.9.100–163`.
- **Regulatory domain** — the channel list comes from the board's country code,
  `CONFIG_HALOW_COUNTRY_CODE` under [`boards/<BOARD>/`](../../boards/). It
  defaults to `"??"` (no channels); set it to your region
  (`AU CA EU GB IN JP KR NZ US`).
