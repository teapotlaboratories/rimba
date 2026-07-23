# rimba-halow-sta

An 802.11ah (Wi-Fi HaLow) **station** example for the ESP32-S3 + Morse Micro
MM6108. It brings up the radio, associates to a HaLow SoftAP over SAE, acquires
an IP, and pings continuously — printing each round-trip time so you can watch
the link on the console.

By **default** it is a **DHCP client**: against the all-ESP mesh-gate
([`rimba-halow-mesh-ap`](../rimba-halow-mesh-ap/)), whose SoftAP serves DHCP on
the flat `10.9.9.0/24`, it leases a `10.9.9.x` and pings a mesh node
(`10.9.9.100`) zero-config — no static IP, no route (the gate L2-bridges and
proxy-ARP resolves). For an AP with **no** DHCP server (the plain
[`rimba-halow-ap`](../rimba-halow-ap/) demo), pass a static IP.

## Build + flash

Run from the repo root; pick the `PORT` your board enumerates on.

**Against the mesh-gate (default — DHCP + ping a mesh node):**

```bash
make flash APP=rimba-halow-mesh-ap BOARD=proto1-fgh100m PORT=/dev/ttyACM0   # the gate
make flash APP=rimba-halow-mesh    BOARD=proto1-fgh100m PORT=/dev/ttyACM1   # a mesh node (10.9.9.100)
make flash APP=rimba-halow-sta     BOARD=proto1-fgh100m PORT=/dev/ttyACM2   # this STA (DHCP)
make monitor APP=rimba-halow-sta   BOARD=proto1-fgh100m PORT=/dev/ttyACM2   # Ctrl-] to quit
```

Watch for `associated to "rimba-ping"`, then `DHCP lease 10.9.9.x`, then
`reply from 10.9.9.100 ...`.

**Against a plain rimba-halow-ap (no DHCP — static IP):**

```bash
make flash APP=rimba-halow-ap  BOARD=proto1-fgh100m PORT=/dev/ttyACM0       # the AP (192.168.12.1)
make flash APP=rimba-halow-sta BOARD=proto1-fgh100m PORT=/dev/ttyACM1 \
      STA_IP=192.168.12.2 PING_IP=192.168.12.1                              # static, ping the AP
```

## Options (make vars → build defines)

- **`STA_IP=a.b.c.d`** — pin a **static** IP instead of DHCP (for an AP with no
  DHCP server, or a fixed-address test). Omit for the DHCP-client default.
- **`PING_IP=a.b.c.d`** — the ping target (default: mesh node `10.9.9.100`).
- **`NO_PING=1`** — responder-only: associate + acquire an IP but originate no
  traffic (used to exercise the gate's mesh→AP broadcast bridging).

## What to change for your own network

Edit the parameters at the top of [`main/app_main.c`](main/app_main.c):

- `LINK_SSID` / `LINK_PSK` — the network name and passphrase (>= 8 chars, required
  for SAE). Keep these in sync with the AP / mesh-gate.
- `CONNECT_TIMEOUT_MS` (how long to wait for association before idling) and
  `PING_INTERVAL_MS` (the ping period) are also defined there if you want to tune them.
