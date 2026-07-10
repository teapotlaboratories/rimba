# ESP32 Mesh + AP concurrency — Stage 1 & Stage 2 code-map (new code ↔ reference)

**Scope.** morselib surgery that lets one MM6108 run an 802.11s **mesh point** and a
**SoftAP** co-channel on the ESP32 (the all-ESP "Mesh-gate"), per the locked design in
[`rimba-mesh-ap-milestones.md` §A3 "ESP32 port plan"](rimba-mesh-ap-milestones.md#esp32-port-plan--mesh--ap-co-channel-on-one-mm6108-derived-from-morse_driver-recon-2026-07-08).
Submodule branch: `components/halow` → `feat/mesh-ap-concurrency`.

Implements **Stage 1** (concurrent vifs in umac: mesh on the primary vif, AP on a secondary
vif) and **Stage 2 per-vif beacon** (mesh + AP beacon at once). `mmwlan_ap_disable` is
**deferred** (see §Deferred). **Stage 3 (on-air bring-up + byte-diff on chronium) is NOT
done** — this doc + a clean build are the deliverables so far.

## Governing-rule status ([[proper-fix-follow-linux]], [[porting-ships-verified-codemap]], [[verify-onair-chronium-monitor]])

- **Derive-from-Linux:** the model mirrors the **proven Linux Mesh-gate recipe** (mesh on
  primary `wlan1`, AP on secondary `ap0`; §A3) and the Linux `morse_driver`
  `iface_combination {AP, MESH}, num_different_channels = 1`.
- **⚠️ Code-map verification caveat:** the **Linux kernel `morse_driver` / `net/mac80211`
  tree is not checked out on this bench machine** (only morselib's own vendored driver port
  under `.../morselib/src/driver/morse_driver/` is present). So the Linux-side references
  below come from the design doc's 2026-07-08 recon, **not** a fresh two-tree grep. Every
  **new-code** `file:line` below WAS grep-verified in this repo. **Re-verify the Linux
  file:line anchors against a checked-out morse_driver tree before merge.**
- **On-air verify:** ☐ pending (Stage 3, needs chronium `morse0` monitor byte-diff vs a live
  Linux mesh+AP gateway).

Paths below are relative to
`components/halow/components/mm-iot-sdk/framework/morselib/src/`.

---

## Stage 1 — concurrent vifs in umac

### 1a. Secondary AP vif slot — `umac/interface/umac_interface_data.h:40-42`
Added `uint16_t ap_vif_id` + `uint8_t ap_mac_addr[]` to `struct umac_interface_data`
(which still holds the single **primary** `vif_id`/`mac_addr`). `ap_vif_id ==
UMAC_INTERFACE_VIF_ID_INVALID (0xffff)` means "no concurrent AP vif" — needed because `0`
is a valid FW-assigned vif id.
- Init to INVALID: `umac_interface.c:59` (`umac_interface_init`) and re-set after the
  full-teardown `memset` at `umac_interface.c:497`.
- **Reference:** the primary/secondary split mirrors the Linux recipe's two netdevs
  (`wlan1` mesh + `ap0` AP) — §A3 "Roles" + [`reference/rimba-linux-node-setup.md`]. In
  Linux each vif is a `struct ieee80211_vif`; here we keep a fixed 2-slot model
  (primary + one AP) rather than an N-vif array (the 90-use blast-radius the design rejected).

### 1b. Allow {AP, MESH} — `umac_interface_type_is_compatible_with_active()`, `umac_interface.c:184-201`
Relaxed the MESH-exclusive gate so adding **AP while MESH is active** is permitted
(`type != UMAC_INTERFACE_MESH && type != UMAC_INTERFACE_AP` at `:193`). The converse
(adding MESH while an AP is active) is still rejected by the existing test at `:196-200`,
which **enforces mesh-first ordering** (mesh must own the primary vif; a mesh on a
secondary vif never joins — §A3 recipe note).
- **Reference:** Linux `morse_driver` publishes `iface_combination` limits allowing
  `{AP, MESH}` with `num_different_channels = 1` (co-channel). Our ordering constraint is a
  host-abstraction detail Linux doesn't have (Linux mesh works on any vif); it encodes the
  empirically-required mesh-primary rule from the §A3 recipe.

### 1c. Allocate a 2nd vif for the concurrent AP — `umac_interface_add()`, `umac_interface.c:230-337,366-375`
- `ap_secondary` discriminator (`:230-231`): `type == AP && (active & MESH)`.
- MAC routing (`:233-258`): a concurrent AP's BSSID goes to `ap_mac_addr` (NOT clobbering
  the mesh's `mac_addr`); if the caller's BSSID is absent/zero/equal-to-mesh, derive a
  distinct locally-administered MAC via `derived[0] ^= 0x02` — the **same convention** as
  `umac_ap_enable`'s fallback BSSID (`umac_ap.c:180`), so the AP vif MAC == the advertised
  BSSID.
- New alloc branch (`:323-337`): `mmdrv_add_if(&data->ap_vif_id, data->ap_mac_addr,
  MMDRV_INTERFACE_TYPE_AP)` — a **second** FW vif, with **no** `rm_if`/`umac_ps_reset`
  (unlike the legacy STA↔AP swap branches at `:338-364`, which stay for the non-mesh case).
- vif routing (`:369`): `used_vif_id = ap_secondary ? ap_vif_id : vif_id` drives both
  `umac_interface_init_vif()` and the `*vif_id` out-param → `umac_ap_data.vif_id`
  (`umac_ap.c:220`) receives the secondary id, so **all 8 `data->vif_id` uses in umac_ap.c
  route to the AP vif for free** (they read the AP subsystem's own stored copy).
- **Reference:** FW `MORSE_CMD_ID_ADD_INTERFACE` returns a FW-assigned `vif_id`
  (`driver/morse_driver/command.*`), the same multi-vif primitive Linux `morse_add_interface`
  uses. Stage 0 (§A3) empirically proved the FW grants a 2nd concurrent MESH-alongside-AP vif.

### 1d. Getters + teardown resolve AP → secondary — `umac_interface.c`
- `umac_interface_get_vif_id(type_mask)` `:508-527`: AP requests resolve to `ap_vif_id`
  when a concurrent AP exists, else the primary. Fixes the 11 getter-callers centrally
  (`umac_ps.c:69`, `umac.c:1397/1406`, `umac_mmdrv_shim.c:72`, `umac_datapath.c`, …).
- `umac_interface_get_vif_type_mask(vif_id)` `:529-552`: `ap_vif_id → {AP}`; the primary vif
  reports its types with **AP masked out** when a secondary AP exists (so a mesh TX-status on
  the primary routes to the mesh/STA supplicant ctx, not the AP ctx — `supplicant_core.c:463`).
- `umac_interface_remove()` `:412-443`: (i) removing a primary type while a concurrent AP is
  up tears the AP down first (`:415-421`, keeps FW state consistent in any order);
  (ii) removing the AP when it's the secondary does a **targeted** `mmdrv_rm_if(ap_vif_id)` +
  TWT deinit and returns **without** the full driver deinit (`:423-441`) so the mesh survives.
- `umac_mesh_tear_down_active_interfaces()` `umac_mesh.c:2988-3001`: dropped `UMAC_INTERFACE_AP`
  from the teardown list so mesh bring-up never destroys a concurrent AP (belt-and-suspenders
  — 1b already rejects AP-then-mesh).
- **Reference:** mirrors Linux teardown ordering (remove the dependent AP vif before the
  mesh vif); `umac_supp_remove_ap_interface` exists for the supplicant side (used later).

---

## Stage 2 — per-vif beacon

Problem: `driver_data.beacon` was a **single slot** and `mmdrv_host_get_beacon()` dispatched
by **global** `umac_mesh_is_active()`/AP state, so only one interface could beacon and the
mesh always won. Fix = make the beacon path per-vif (the FW already exposes one beacon IRQ
per vif: `MORSE_INT_BEACON_BASE_NUM (17)` + `vif_id`, mask `GENMASK(24,17)` = 8 vifs,
`driver/morse_driver/hw.h:74-75`).

### 2a. Per-vif beacon state — `driver/morse_driver/morse.h:204-206`
`beacon.{vif_id,enabled}` → `uint8_t enabled_vif_mask` + `volatile
atomic_uint_least32_t pending_vif_mask` (kept `count`, `beacon_work_fn`).

### 2b. ISR latches which vif fired — `morse_beacon_irq_handle()`, `beacon.c:12-33`
For each enabled beacon vif, test its IRQ bit in `status1_reg`; `atomic_fetch_or` the set
of fired vifs into `pending_vif_mask` and wake the beacon work once. (Race-safe: ISR does
atomic OR, task does atomic exchange.)

### 2c. Work drains all pending vifs — `morse_beacon_work_()` + `morse_beacon_tx_one()`, `beacon.c:55-104`
`atomic_exchange(pending_vif_mask, 0) & enabled_vif_mask`, then for each set vif call
`mmdrv_host_get_beacon(vif)` and enqueue on the shared beacon TC queue (the beacon mmpkt is
already vif-tagged in its tx metadata — `umac_ap.c:346`, mesh equivalent — so one queue is fine).

### 2d. Per-vif start/stop — `beacon.c:107-149`, `beacon.h:16`, `driver.c:709`
`morse_beacon_start(vif)` sets the vif's `enabled_vif_mask` bit + kicks its first beacon;
`morse_beacon_stop(vif)` (now takes a `vif_id`) clears just that vif. `mmdrv_rm_if`
(`driver.c:709`) stops beaconing only for the vif being removed. Init sentinel dropped
(`driver.c:345` — the `memset` zeroes the masks).

### 2e. vif-aware dispatch — `mmdrv_host_get_beacon(uint16_t vif_id)`, `umac_mmdrv_shim.c:209-238` (proto `internal/mmdrv.h:1450`)
IBSS stays global (exclusive). Otherwise map `vif_id → type` via
`umac_interface_get_vif_type_mask()`: MESH → `umac_mesh_get_beacon`, AP →
`umac_ap_get_beacon`; a legacy global fallback covers an empty lookup (beacon in flight
during teardown).
- **Reference:** Linux builds the beacon per `ieee80211_vif` in the driver's `.tx` /
  beacon-get callback; this reproduces that per-vif selection on the host side.

---

## Deferred

- **`mmwlan_ap_disable()`** — still a stub (`umac.c:1299`, annotated). Runtime AP teardown
  is **not** on the mesh+AP bring-up path (the gateway runs both interfaces continuously),
  and a correct version must order `umac_supp_remove_ap_interface`
  (→ `wpa_supplicant_remove_iface`) vs `umac_interface_remove(UMAC_INTERFACE_AP)` to avoid a
  double vif-remove / double-free — that ordering needs **on-air validation** before it
  ships. The concurrent-AP secondary vif itself IS torn down correctly by
  `umac_interface_remove()` (§1d).

## Build validation

`make build APP=rimba-halow-mesh BOARD=proto1-fgh100m` → **Project build complete**, no
errors/warnings in the changed files (`rimba-halow-mesh` sets `CONFIG_HALOW_AP_MODE=y`, so
libmorse compiles all the changed umac/AP/beacon sources). NB: `BOARD=proto1` currently
fails CMake config on a **missing `bcf_mf16858` BCF** in the freshly-pinned 1.17.8 firmware
— a pre-existing vendor-pin mismatch, unrelated to these changes; `proto1-fgh100m`
(`bcf_fgh100mhaamd`, present) builds.

## Next (Stage 3 — not started)

Build a dedicated mesh+AP app (mesh_start first, then AP), bring it up on one MM6108, and on
chronium's `morse0` monitor **byte-diff** the ESP mesh beacon and AP beacon against a live
Linux gateway; then route an ESP STA under the AP through the mesh to a 2nd node (the §A3
recipe, all-ESP). Follow [[verify-onair-chronium-monitor]] + [[radio-silent-after-every-test]].
