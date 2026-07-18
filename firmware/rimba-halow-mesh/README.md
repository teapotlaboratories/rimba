# rimba-halow-mesh — 802.11s secured mesh point

Brings up a single 802.11s mesh interface on the MM6108 (Mesh ID + Mesh
Configuration IEs), joins a **secured** mesh (SAE + AMPE + CCMP), pins a static
mesh IP, and logs its established peers. Peering, forwarding, and HWMP path
selection all happen inside morselib — the peer link forms automatically when a
neighbour is heard. This is the minimal secured-mesh-point example the other
mesh apps build on.

## Build + flash

Run from the repo root. Pick the `PORT` your board enumerates on
(`/dev/ttyACM*`):

```bash
make build APP=rimba-halow-mesh BOARD=proto1-fgh100m
make flash APP=rimba-halow-mesh BOARD=proto1-fgh100m PORT=/dev/ttyACM0
make monitor APP=rimba-halow-mesh PORT=/dev/ttyACM0        # serial console, Ctrl-] to quit
```

Mesh reuses the AP-type interface + beacon path, so this app's
`sdkconfig.defaults` sets `CONFIG_HALOW_AP_MODE=y`.

## Pairing with another node

Flash this same app to a second board (or run it alongside a Linux HaLow mesh
node on the same Mesh ID + channel). Each node derives a unique mesh MAC from
its ESP32 efuse MAC, so no per-board config is needed. When two nodes are on
air, the heartbeat prints a rising `estab_peers` count and the peer MACs:

```
I (…) rimba-mesh: mesh alive, uptime=25s  estab_peers=1
I (…) rimba-mesh:   peer[0]=e2:72:a1:f8:ef:a4
```

## What to change for your own network

Edit the `#define`s at the top of [`main/app_main.c`](main/app_main.c):

- **`MESH_ID`** — the mesh network name (default `"rimba-mesh"`). All nodes in
  one mesh must share it. Change for your network.
- **`MESH_S1G_CHAN`** — the S1G channel (default `27`, US 915.5 MHz, 1 MHz BW).
  Set it to a channel valid for your regulatory domain; all nodes must agree.
- **Regulatory domain** — the channel list comes from the board's country code,
  `CONFIG_HALOW_COUNTRY_CODE` under [`boards/<BOARD>/`](../../boards/). It
  defaults to `"??"` (no channels), so set it to your region
  (`AU CA EU GB IN JP KR NZ US`).
- **`MESH_GATE_IP`** — commented out by default; a plain single-subnet mesh pins
  no gateway. Define it as a mesh gate's IP only if this node must reach hosts on
  another subnet through that gate.

The static mesh IP is derived from the node's mesh MAC as `10.9.9.<100 + low
bits>` (a convention that gives each node a distinct address without a DHCP
server); change the `10.9.9.` prefix / netmask in `mesh_net_task()` for your own
subnet.

The SAE mesh password is a compile-time `#define MESH_SAE_PASSWORD` in the
morselib mesh component (`components/halow/.../umac/mesh/umac_mesh.c`), shared by
every node in the mesh — change it there if you want your own passphrase.

