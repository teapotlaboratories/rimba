# 2026-07-30 — A standalone examples repo, bench-verified; and why the mesh-gate cannot scale to 255 clients

**Goal.** Give the `mm-esp32-halow` fork examples for the features it adds, since `components/halow/examples/`
carried only Morse Micro's upstream six and none of them touch mesh, IBSS, TWT or the gate.

**Outcome.** Five examples now live in a **new repo, `teapotlaboratories/esp-halow-examples`** (cloned at
`../esp-halow-examples`), all five verified on the bench — not merely compiled. PR #1 merged, PR #2 open.
One example was **100% non-functional despite building cleanly**, which is the main argument this worklog
makes. Along the way, scaling the mesh-gate's client count turned out to be blocked on RAW, not on tables.

---

## 1. Why the examples are not in the submodule

They were written into `components/halow/examples/` first and **reverted** at the owner's request. Do not
put them back. An upstream Morse example is a bare `CMakeLists.txt` plus a `main/idf_component.yml` that
resolves `morsemicro/halow` from the ESP Component Registry at `^2.10.4-esp32-2` — so `idf.py build`
inside the submodule downloads the *published* 2.10.4 and ignores the fork it is sitting in.

Every fork feature postdates the pristine import `68ddbd8d`, so such an example does not merely misbehave,
it **fails to compile**:

| Needs | Added by |
|---|---|
| `umac/mesh/umac_mesh.h` | `c16e9a8a` |
| `umac/ibss/umac_ibss.h` | `f4778430` |
| `mmwlan_twt_setup_request` | `eeb15af9` |
| `mmwlan_{ampdu_capability_advertised,twt_agreement_installed}` | `3c8ded0d` |
| `CONFIG_HALOW_AP_MAX_STAS` | `6a698edf` |

The standalone repo resolves the component locally (`EXTRA_COMPONENT_DIRS` + `morse_firmware_generate_component()`)
and deletes the registry manifests, which is what makes the examples buildable at all.

## 2. What the new repo is

Rimba's Makefile system, narrowed: `make build/flash/monitor APP=<example> BOARD=<board>`, out-of-source
`build/<APP>/<BOARD>/`, a board overlay supplying the SPI pins and country code. Submodules pinned to
exactly Rimba's revisions — `components/halow c9f3a572`, `vendor/esp-idf v5.4.2` (shallow, 620 M),
`vendor/morse-firmware 1.17.8`.

Two deliberate differences from Rimba:

- **The toolchain is repo-local.** `IDF_TOOLS_PATH := $(CURDIR)/.espressif` (~3.8 GB, gitignored) so the
  checkout is self-contained and other ESP-IDF projects keep their own `~/.espressif`.
  `make install-toolchain` also pip-installs `cmake==3.30.5` + ninja into that venv, which ESP-IDF does
  not bundle on Linux. Fresh clone to built binary is two commands.
- **`IDF_PATH` uses `:=`, not `?=`.** `?=` defers to the *environment*, and ESP-IDF's `export.sh` exports
  `IDF_PATH` — so in any shell where Rimba had been sourced, the examples repo silently built against
  Rimba's ESP-IDF instead of its own pinned one. Verified before and after. **Rimba's own Makefile still
  has this bug** (`Makefile:17`).

## 3. The bench run — and the defect only hardware could find

All five run on the proto1-fgh100m bench (MM6108 fw 1.17.8) plus chronite for interop:

| Example | Result |
|---|---|
| `ibss` | cell up 2.3 s, roles self-assigned, bidirectional ping 95/98 |
| `mesh` | peers ESTAB ~5 s; **Linux 802.11s interop both ways** — chronite peered with both ESPs, pinged 10/10 and 9/10, ESP reported `established_peers=2` incl. the Linux MAC |
| `twt_powersave` | associated 6.1 s, AP saw `aid=1`, **TWT INSTALLED on flow 0**, held 66 s |
| `ap_scale` | `max_stas=255`, per-STA state in PSRAM, real client authorized, heap flat |
| `mesh_gate` | client DHCPs `10.9.9.2`, reaches a mesh node **13/15 at TTL=64** — judged by our own `test-mesh-gate-sta` fixture, which reported PASS against the example acting as gate |

**`twt_powersave` shipped `CONFIG_HALOW_PS_MODE=y` and the station never associated** — a 30 s timeout,
every time, while compiling and linking perfectly. An A/B on wired vs unwired boards isolated it: on
board2 (BUSY/WAKE wired) the same binary associates in 5.7 s with the option on. So **power-save across
the SAE handshake is harmless; the missing wiring was the whole cause**, and an initially plausible
theory about the handshake was simply wrong. Not a component bug — its Kconfig states the precondition.
Recorded in [[halow-ps-mode-needs-busy-wake]].

Note the latent trap this exposed: `boards/proto1-fgh100m/sdkconfig.defaults` declares `CONFIG_MM_WAKE=2`
and `CONFIG_MM_BUSY=5` for **every** board on that overlay, while only board2 is physically wired.

## 4. Two dead Kconfig symbols, and a wrong comment in our own code

Morse's own examples carry `CONFIG_LWIP_NETIF_LINK_CALLBACK` (in `softap/`) and
`CONFIG_LWIP_ETHARP_TRUST_IP_MAC` (in `iperf/`). Both are silently ignored; the latter was **removed from
lwIP** as insecure.

That matters here because **`firmware/rimba-halow-mesh-ap` credits that dead option** for why its
proactive proxy-ARP push works. The real mechanism: each push is addressed *to* the host being taught, so
`etharp_input` sees `for_us` and uses `ETHARP_FLAG_TRY_HARD` (creates an entry) rather than
`ETHARP_FLAG_FIND_ONLY` (update-only) — `etharp.c:702`. Worth correcting in Rimba.

## 5. Splitting the gate (PR #2)

`mesh_gate/main/app_main.c` was 1013 lines, ~75 % of it infrastructure identical for any gate. Split into
`app_main.c` (182) / `gate_netif.c` (252) / `gate_bridge.c` (342) / `gate_arp.c` (375) / `gate.h` (111),
re-verified against the same unmodified fixture (14/15, TTL=64 — the delta from 13/15 is bench variance,
not claimed as improvement).

Three real changes: the **proxy-ARP table is now locked** (three concurrent accessors, previously
unguarded; the push snapshots under the lock and transmits after), **introspection** was added (the gate
could not report learned state without editing source), and a **`_Static_assert`** couples
`EXAMPLE_AP_MAX_STAS` to `GATE_MAX_CLIENTS` — because only the mesh→AP direction consults the client
table, a client past its end would associate, take a lease, send into the mesh, and **silently never
receive a reply**.

## 6. Why the gate cannot match `ap_scale`'s 255 clients

Asked whether the gate's ceiling could match `ap_scale`. It cannot, and the reason is not table sizes.

`ap_scale` registers **0** rx callbacks, runs **0** DHCP servers and does **0** per-client lookups — the
default netif handles frames and the app only counts associations. The gate does 2 / 1 / 4. Every client
is per-frame work.

Three blockers, none of which a higher MCS fixes:

1. **The proactive proxy-ARP push is O(n_ap × n_mesh)** — one unicast per pair every 3 s. 255 clients ×
   5 mesh nodes = 2550 frames/period. **Per-frame overhead dominates, not bit rate** (calculated):
   1 MHz MCS0 ≈ 7.6 s, 1 MHz MCS9 ≈ 3.2 s — *still* over the 3 s period — 8 MHz MCS7 ≈ 1.4 s, i.e. ~47 %
   duty cycle on ARP upkeep alone. A 13× rate rise buys ~2.4× because the 1 MHz preamble is a fixed
   ~0.6 ms floor. Must be made lazy at any PHY, and it binds from ~30 clients.
2. **EDCA contention** with many STAs is a collision problem, not a bitrate problem. The 802.11ah answer
   is **RAW** — unimplemented, and gated on an unstarted on-air S0 spike.
3. **Addressing caps at 98, not 253.** Mesh nodes self-assign `.100 + (mac[5] & 0x3f)` so they own
   `.100–.163`; DHCP hands out one contiguous range from `.2`, so it stops at `.99`.

Also: MCS is not settable in production — the only override is `mmwlan_ate_override_rate_control()` (ATE
= test equipment), and the bench runs 1 MHz (`op_class=68`, `s1g_prim_chwidth=0`), the
longest-range/lowest-rate config. Higher MCS means shorter range, trading away why HaLow was chosen.

**Realistic gate target is 16–32 clients.** `ap_scale`'s 255 is a *structural* claim (TIM encoding, table
sizing) proven with one client, not a capacity claim. Detail in [[mesh-gate-scaling-blocked-on-raw]].

## 7. Bench notes

- The midnight `daily-reboot.timer` fired mid-session: killed the PPK2 holder, wiped `/tmp` (tmpfs), and
  board2 came back as a **different `ttyACM`** — resolve by efuse MAC, always.
- `/tmp` is a **1.9 GB tmpfs**; a second full submodule clone exhausted it and produced a wall of
  `unable to write file` errors that looked like repo corruption and was not.
- Two familiar footguns bit again: a stale `dependencies.lock` pointing into a deleted build dir breaks
  the next configure with an opaque manifest error (now handled in the examples repo's Makefile), and
  `CMakeCache` made `test-mesh-gate-sta`'s `STA_IP`/`NO_PING` sticky, so it silently came up in
  responder-only mode.
- Bench left radio-silent after every run.

## 8. Open

- **PR #2** (gate split) awaiting review/merge.
- Rimba fixes this surfaced: the `IDF_PATH ?=` env-hijack (`Makefile:17`); the dead-Kconfig comment in
  `rimba-halow-mesh-ap`; `rimba-halow-mesh/README.md:48` documents a `MESH_GATE_IP` knob deleted in
  `f2e835b`; `firmware/README.md` lists six apps that no longer exist; `docs/rimba-todo.md:26` still
  records RISK-02 as ≈1.39 s against the ~1.7 s the new rig measured.
- Untested example configurations: `EXAMPLE_TWT_ACTION_FRAME`, `EXAMPLE_STA_WNM_SLEEP`,
  `EXAMPLE_MESH_LEAF`, `EXAMPLE_MESH_AMPDU=n`. No metered power claim anywhere.
- **Next:** the RAW AP-side S0 spike, which gates both the RAW feature and any many-client gate story.
