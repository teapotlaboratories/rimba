# rimba-halow-ibss — ad-hoc (IBSS) HaLow example

Brings up an IBSS (ad-hoc) HaLow cell with no access point: every node beacons,
discovers its peers, derives a static IP from its own MAC
(`192.168.13.<octet>`), and pings each peer it hears. Flash the same binary onto
every board — one node creates the cell, the rest join — and they form a flat
peer-to-peer network that also interoperates with a Linux HaLow node on the same
BSSID.

## Build + flash

Run from the repo root. Pick the `PORT` your board enumerates on
(`/dev/ttyACM*`):

```bash
make build APP=rimba-halow-ibss BOARD=proto1-fgh100m
make flash APP=rimba-halow-ibss BOARD=proto1-fgh100m PORT=/dev/ttyACM0
make monitor APP=rimba-halow-ibss PORT=/dev/ttyACM0        # serial console, Ctrl-] to quit
```

## Pairing with the other examples

IBSS needs at least two nodes on the same cell, so flash the same app onto two
or more boards (each on its own `PORT`):

```bash
make flash APP=rimba-halow-ibss BOARD=proto1-fgh100m PORT=/dev/ttyACM0
make flash APP=rimba-halow-ibss BOARD=proto1-fgh100m PORT=/dev/ttyACM1
```

Every node keeps the link parameters identical (SSID, BSSID, S1G channel) but
gets a distinct MAC, so each derives a different IP and pings the others. On the
console you should see `peer_cb ADDED ...` when a node is discovered and a
repeating `reply from 192.168.13.x ...` once the ping is flowing.

- **`rimba-halow-scan`** — flash it on a spare board to confirm the IBSS nodes
  are on air; their beacons show up in the scan list.

## What to change for your own network

The link parameters are `#define`s at the top of
[`main/app_main.c`](main/app_main.c) — every node in the cell must use the same
values:

- **`LINK_SSID`** — the cell name (`"rimba-ibss"` by default). Change it for
  your own network.
- **`LINK_BSSID`** — the pre-shared cell BSSID. This is a provisioned network:
  every node ships knowing the BSSID, so there is no TSF merge. Provision your
  own locally-administered address here (the `0x02` first octet) rather than
  shipping the literal.
- **`LINK_S1G_CHAN`** — S1G channel (27 = US 915.5 MHz, 1 MHz BW). Set it for
  your region's band plan; it must match the regulatory domain below.
- **`IP_PREFIX`** / **`NETMASK`** — the ad-hoc subnet. The host octet is taken
  from the last byte of each node's MAC, so no DHCP is involved.
- **`CREATOR_MAC_BIT`** — how a node decides whether to CREATE the cell or JOIN
  it. The default self-assigns roles from a MAC bit so one flat binary needs no
  per-node config; if you provision roles yourself, set `creator` from your own
  policy instead.

Also set the board's regulatory domain: `CONFIG_HALOW_COUNTRY_CODE` under
[`boards/<BOARD>/`](../../boards/) defaults to `"??"`, which reports no channels
and the radio will not come up — set it to your region
(`AU CA EU GB IN JP KR NZ US`).
