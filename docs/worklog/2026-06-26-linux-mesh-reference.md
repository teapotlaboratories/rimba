# 2026-06-26 — Linux morse 802.11s mesh: working reference (beacon → peer → data)

Brought the **Linux morse stack** up as a fully working 802.11s HaLow mesh and confirmed
two Linux nodes **peer and pass IP traffic**. This is the reference behaviour the ESP
(morselib) mesh must match for P2 (peering / MPM). It also unblocks the IBSS port: the same
morse code path (`morse_ibss_mesh_setup_freq`) and config requirements apply to IBSS.

Bench: **chronium** `192.168.7.187` (MAC `3c:22:7f:37:50:42`) and **chronite**
`192.168.7.191` (MAC `3c:22:7f:37:51:38`), both RPi5 + MM6108, same 1.17.8 morse stack,
kernel 6.12.21. S1G channel 27 → 5 GHz-model ch112 / `freq 5560` (915.5 MHz, 1 MHz BW, US).

## Result

```
# chronium                                   # chronite
Station 3c:22:7f:37:51:38 (on wlan1)         Station 3c:22:7f:37:50:42 (on wlan1)
    signal: -20 dBm                              signal: -19 dBm
    mesh plink: ESTAB                            mesh plink: ESTAB

# ping over the mesh (10.9.9.1 -> 10.9.9.2): 5/5 received, 0% loss,
# rtt min/avg/max 4.77/18.18/65.8 ms  (first pkt 65 ms = HWMP path discovery)
```

## How to bring up morse Linux mesh (the recipe)

morse Linux mesh is **driven by `wpa_supplicant_s1g`, NOT by `iw … mesh join`.** This is the
single most important fact. The morse driver comment (`mac.c`, `bss_info_changed`,
`BSS_CHANGED_BEACON_ENABLED`) says it outright: *"Start mesh will be handled when supplicant
configures mesh id and other params."* The mesh START + beaconing command
(`MORSE_CMD_ID_MESH_CONFIG`, opcode START, `enable_beaconing`) is only sent once the morse
mesh conf has a `mesh_id_len` — and that is populated by a **morse vendor command from
wpa_supplicant** (`command.c` → `morse_cmd_set_mesh_config` → `mesh.c` sets `mesh_id_len`).
Bare `iw mesh join` sets the mesh id in mac80211 but never sends the morse vendor command, so
the firmware mesh BSS never starts → **no beacon, and the firmware never enables RX.**

Working `wpa_supplicant_s1g` mesh config (open mesh, matches the ESP `rimba-mesh`):

```conf
ctrl_interface=/var/run/wpa_supplicant_s1g
country=US
user_mpm=1
network={
    ssid="rimba-mesh"
    mode=5                       # WPAS_MODE_MESH
    key_mgmt=NONE                # open mesh
    country="US"                 # MUST be quoted (STR field; unquoted parses as hex)
    op_class=68                  # S1G global op-class for ch27 / 1 MHz
    channel=27                   # S1G channel (NOT a 5 GHz freq)
    s1g_prim_chwidth=0
    s1g_prim_1mhz_chan_index=0
    dtim_period=1                # REQUIRED — see bug below
    beacon_int=100
}
```

Run:
```sh
export LC_ALL=C; export PATH=$PATH:/usr/sbin:/usr/local/bin
sudo iw dev wlan1 mesh leave 2>/dev/null; sudo pkill -9 -f wpa_supplicant_s1g
sudo rm -f /var/run/wpa_supplicant_s1g/wlan1          # clear stale ctrl socket
sudo ip link set wlan1 down; sudo iw dev wlan1 set type managed; sudo ip link set wlan1 up
sudo wpa_supplicant_s1g -B -D nl80211 -i wlan1 -c /tmp/wpa-mesh.conf -f /tmp/wpa-mesh.log
# expect: wlan1 type "mesh point", channel 112 (5560), log "MESH-GROUP-STARTED"
# verify beaconing: page_stats "Beacon Tx" climbs (~9.8/s); peers: iw dev wlan1 station dump
```
Do **not** set a `frequency=` line — morse derives the HT/5 GHz freq from
`country`+`op_class`+`channel` (`s1g oper class 68 → S1G ch27 → HT chan 112 → 5560`).

## Two morse-fork wpa_supplicant pitfalls (cost most of the debugging)

Both live in the morse hostap fork (`~/halow/hostap/wpa_supplicant`), under
`#ifdef CONFIG_IEEE80211AH`.

1. **`morse_ibss_mesh_setup_freq()` segfaults (SIGSEGV) if the S1G fields are missing.**
   `wpa_supplicant.c:~2948 morse_ap_configure_channelization(ssid->country, ssid->op_class)`
   dereferences a NULL in the channelization table when `country` is empty / `op_class` 0.
   Backtrace: `morse_ibss_mesh_setup_freq` ← `wpa_supplicant_join_mesh (mesh.c:713)` ←
   `wpa_supplicant_associate`. Fix: always provide `country`/`op_class`/`channel` in the
   network block. (This function serves **both IBSS and mesh** — same requirement for IBSS.)

2. **DTIM != 1 is rejected, not corrected.** `mesh.c:766-773`
   (`wpa_supplicant_join_mesh`) logs `"Invalid DTIM period (%d) for Mesh, set (1)"` then does
   `ret = -1; goto out` — the message lies; it does NOT set 1, it bails. With no
   `dtim_period` configured it defaults to 0 → every join fails with "Could not join mesh".
   Fix: set `dtim_period=1` explicitly. (Candidate upstream fix: actually set
   `params->dtim_period = 1` instead of erroring — left as a backlog item, config works.)

## Correction: the "CONFIG_MORSE_MONITOR breaks RX" conclusion was WRONG

Earlier sessions concluded the monitor build broke normal-vif RX. **It does not.** The real
cause of the "deaf" nodes was the same as above: **no active BSS → firmware RX disabled.**
Bare `iw mesh join` never starts the morse mesh, so the radio never listens — looked like
broken RX. With a real `wpa_supplicant_s1g` mesh, **chronium peered with chronite at -20 dBm
while running the monitor (`CONFIG_MORSE_MONITOR=y`) build.** So the monitor build is fine for
normal mesh/AP/STA operation.

Caveat that *is* real: the `morse0` **raw monitor tap delivers 0 frames** here (capture
returned nothing even with an active mesh and the radio on-channel). Not needed for mesh dev —
`iw dev wlan1 station dump` / `iw dev wlan1 mpath dump` and the Linux nodes themselves give us
the reference. `morse0` raw-tap delivery is a separate, still-open issue (low priority).

## Implication for the ESP side (P2)

The ESPs (`rimba-halow-mesh`, board0/board1) **beacon** `rimba-mesh` with real per-node MACs
and hear each other, but **do not form a peer link** with the Linux nodes — because ESP-side
MPM (mesh peering action frames: Open / Confirm / Close) is the P2 work still in progress, not
a morse/Linux incompatibility. We now have a fully working Linux peer (chronium↔chronite) to
diff against: capture/observe the Mesh Peering Management frames and the Mesh Configuration
element (active path sel = HWMP, metric = airtime, sync = neighbour-offset) the Linux nodes
exchange, and match them in morselib's MPM.

## State at end of session
- chronium + chronite: both running `wpa_supplicant_s1g` open mesh `rimba-mesh`, **plink
  ESTAB**, IPs `10.9.9.1` / `10.9.9.2`, ping 0% loss. (Runtime only — re-run the recipe after
  reboot; not yet a service.)
- chronium kernel module rebuilt + installed with `CONFIG_MORSE_MONITOR=y` (morse0 present but
  raw tap empty); normal mesh vif RX confirmed working on this build.
- ESP boards: board0/board1 beaconing `rimba-mesh`; no peer yet (P2 MPM pending).
