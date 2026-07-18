# rimba-halow-mesh-ap — the mesh-gate (mesh + AP + L3 routing on one radio)

Runs an 802.11s HaLow **mesh** and a co-channel **SoftAP** concurrently on a
single MM6108, and **routes** IP traffic between the two subnets — the all-ESP32
"mesh-gate". Stations that associate to the SoftAP reach nodes on the mesh (and
back) through this board's lwIP forwarder. The mesh runs on the primary vif, the
AP on a secondary vif; each gets its own `esp_netif`, RX is demuxed per vif, and
TX is tagged with the originating vif.

Two subnets meet here:

- **mesh** `10.9.9.<gw>/24` — primary vif; the host part is derived from the
  board's MAC, printed at boot as `mesh netif static IP 10.9.9.x (gateway host)`.
- **AP** `192.168.12.1/24` — secondary vif, with a DHCP server so associating
  stations are zero-config.

Needs the `components/halow` mesh + AP concurrency support and
`CONFIG_LWIP_IP_FORWARD=y` (already set in this app's `sdkconfig.defaults`).

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
demo — an STA under the AP pinging a node on the mesh:

```bash
# board 0 — this gate (mesh 10.9.9.<gw> + AP 192.168.12.1)
make flash APP=rimba-halow-mesh-ap BOARD=proto1-fgh100m PORT=/dev/ttyACM0

# board 1 — a mesh peer on the same Mesh ID + channel
make flash APP=rimba-halow-mesh    BOARD=proto1-fgh100m PORT=/dev/ttyACM1

# board 2 — a station that joins the AP (static 192.168.12.2, gw 192.168.12.1)
make flash APP=rimba-halow-sta     BOARD=proto1-fgh100m PORT=/dev/ttyACM2
```

For the return path to work, the **mesh peer** needs a route back to the AP
subnet through the gate. In [`rimba-halow-mesh`](../rimba-halow-mesh/), set its
`MESH_GATE_IP` to the gate's mesh IP (the `10.9.9.x` the gate prints at boot);
that installs `192.168.12.0/24 via 10.9.9.<gw>` so replies find their way home.

With all three up, the STA pings its target (a `10.9.9.x` mesh node) and the
round-trips traverse the gate. Watch the gate's console for the STA authorizing
(`AP client joined: …`) and the `gateway ready` line.

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
- **`AP_SUBNET_IP`** / **`AP_SUBNET_MASK`** — the SoftAP subnet handed to
  stations (default `192.168.12.1/24`).
- **Mesh subnet** — the mesh IP is derived from the board's MAC as
  `10.9.9.<100 + low bits>`, gateway `10.9.9.1`. Change the `10.9.9.` prefix /
  netmask in `mesh_net_task()` for your own subnet.
- **Regulatory domain** — the channel list comes from the board's country code,
  `CONFIG_HALOW_COUNTRY_CODE` under [`boards/<BOARD>/`](../../boards/). It
  defaults to `"??"` (no channels); set it to your region
  (`AU CA EU GB IN JP KR NZ US`).
