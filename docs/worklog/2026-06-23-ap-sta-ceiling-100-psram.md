# Worklog — 2026-06-23 — AP STA-count ceiling → 100, per-STA state in PSRAM (HaLow AP)

**Author:** Aldwin (with Claude Code)
**Phase:** scaling the HaLow SoftAP (relay-AP gateway topology) past a handful of leaves.
**Goal:** make the AP's max-associated-STA count and the per-STA RAM placement
**configurable from sdkconfig**, allocate per-STA state **strictly from PSRAM**, and
**raise the structural ceiling to 100** associated stations.
**Status:** code complete + build-verified at cap=100 / PSRAM=y. Hardware validation at
high STA counts deferred (only 3 boards on the bench).

Self-contained record. ESP32 = XIAO ESP32-S3 + FGH100M (`BOARD=proto1-fgh100m`), 8 MB
octal PSRAM, morselib fw 1.17.6 (mm-iot-sdk 2.10.4-esp32-2). All morselib edits are in the
`components/halow` submodule fork.

---

## Why this matters

The Mesh+AP gateway topology (see [`2026-06-22-mesh-ap-twt.md`](2026-06-22-mesh-ap-twt.md))
puts leaves under a relay-AP. That only scales if the AP can actually hold many leaves.
Two things bound that on the ESP32 side:

1. **A hard STA-count ceiling** baked into morselib's S1G TIM encoding (the partial
   virtual bitmap). The shipped code asserts a *single* TIM block → AID < 64 → at most
   63 STAs, and the vendor left a tripwire (`"Review the TIM logic if changing this
   limit"`) daring anyone to raise it.
2. **RAM placement.** Each associated STA costs ~912 B of `umac_sta_data` plus buffers.
   By default small allocations land in internal SRAM (below
   `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`, 16 KB), which is the scarce pool. At 100 STAs
   that's ~90 KB of per-STA state that must not come from internal SRAM.

---

## 1. The STA-count ceiling is the S1G TIM partial-virtual-bitmap encoding

morselib advertises buffered downlink per-AID in the beacon's S1G TIM element using a
**partial virtual bitmap (PVB)**. The PVB is a sequence of *blocks*; each block covers
**64 AIDs** and is encoded as `[block-control][block-bitmap][present subblocks...]`
(802.11ah §9.4.2.6 S1G-style hierarchical bitmap).

The relevant code is `src/umac/ap/traffic_bitmap.h` (the bitmap storage + AID validity)
and `src/umac/ies/s1g_tim.c` (`ie_s1g_tim_build`, the encoder).

What pinned it to 63 STAs was **not** the encoder logic — it was a derived size and a
tripwire assert:

```c
/* traffic_bitmap.h (shipped) */
#define MAX_SUPPORTED_AID    64                              /* one block */
#define S1G_BITMAP_SUBBLOCKS ((MAX_SUPPORTED_AID + 7) / 8)   /* = 8 */

/* s1g_tim.c, inside ie_s1g_tim_build (shipped) */
MAX_ENCODED_BLOCKS   = ceil(S1G_BITMAP_SUBBLOCKS / 8) = 1
MAX_SUPPORTED_PVB_LEN = MAX_ENCODED_BLOCKS * (2 + 8) = 10
MM_STATIC_ASSERT(MAX_SUPPORTED_PVB_LEN == 10, "Review the TIM logic if changing this limit");
```

**Key finding:** the encoder body is *already generic over multiple blocks*:

```c
for (size_t block = 0; block < MAX_ENCODED_BLOCKS; ++block) {
    ...
    DOT11_TIM_BLOCK_HDR_SET_BLOCK_OFFSET(*block_control, block);  /* per-block offset */
    pvb_len += block_size;
}
```

It loops over `MAX_ENCODED_BLOCKS`, emits an empty-block skip, and writes the absolute
block offset into each block-control byte. So the "single block" limitation is purely the
`MAX_SUPPORTED_AID = 64` derivation plus the `== 10` tripwire. The vendor's comment is a
"did you check the surrounding invariants?" gate, not a sign the logic is broken.

I checked the surrounding invariants for two blocks (AID up to 127):

| Invariant | Source | Two-block (AID=128) | OK? |
|---|---|---|---|
| Block-offset field width | `AID_BLOCK_OFFSET_MASK = 0x1f << 6` (5 bits, 0..31) | block 1 used | ✓ |
| TIM page index | `MM_STATIC_ASSERT(MAX_SUPPORTED_AID < 512, ...)`; page = 2048 AIDs | 128 < 512, all in page 0 | ✓ |
| PVB fits the block size | line 474 `pvb_len <= S1G_TIM_MAX_BLOCK_SIZE - 5` (256-5=251) | max pvb_len = 20 | ✓ |
| PVB ≤ spec limit | `MAX_SUPPORTED_PVB_LEN <= MAX_PVB_LEN` (251) | 20 ≤ 251 | ✓ |
| Bitmap storage | `umac_ap_data.h: bitmap[S1G_BITMAP_SUBBLOCKS]`, asserts `*8 >= MAX_SUPPORTED_AID` | 16 B → 128 ≥ 128 | ✓ |
| Cap vs AID space | `umac_ap_data.h: MMWLAN_AP_MAX_STAS_LIMIT < MAX_SUPPORTED_AID` | 100 < 128 | ✓ |

The **parse** side (`ie_s1g_tim_block_bitmap_has_aid` / `_olb_has_aid`, used when *we* are a
STA reading an AP's TIM) was already fully multi-block — it reads `block_offset` and
`page_index` and walks N blocks. So nothing to change there.

### The off-by-one that makes "100" actually 100

`umac_ap.c` reserves AID 0 for the common/broadcast pseudo-STA (`stas[0] = sta_common`):

```c
if (args->max_stas > MMWLAN_AP_MAX_STAS_LIMIT) { error; }     /* line 71 */
data->max_stas = 1 + (args->max_stas ? args->max_stas : MMWLAN_DEFAULT_AP_MAX_STAS);  /* line 98 */
data->stas = calloc(data->max_stas, sizeof(struct umac_sta_data *));
...
if (aid == 0 || aid >= data->max_stas) reject;                /* line 513: AID in 1..max_stas-1 */
```

So a configured cap of `N` yields usable AIDs `1..N` = exactly **N** stations, and the
largest AID is `N`. For N=100 the largest AID is 100, which needs `MAX_SUPPORTED_AID > 100`
→ 128 (two blocks). The structural max is `MMWLAN_AP_MAX_STAS_LIMIT <= 127`.

---

## 2. Changes made (all in the `components/halow` submodule unless noted)

**a. Raise the AID space to two TIM blocks** — `src/umac/ap/traffic_bitmap.h`:
```c
#define MAX_SUPPORTED_AID    128   /* was 64; two PVB blocks, AIDs 1..127 */
```
This auto-recomputes `S1G_BITMAP_SUBBLOCKS` (8→16), `MAX_ENCODED_BLOCKS` (1→2) and
`MAX_SUPPORTED_PVB_LEN` (10→20) in the encoder.

**b. Update the tripwire assert** — `src/umac/ies/s1g_tim.c`:
```c
MM_STATIC_ASSERT(MAX_SUPPORTED_PVB_LEN == 20, "Review the TIM logic if changing this limit");
```
Kept as an exact-value tripwire (not relaxed to `<=`) so a *future* AID bump still forces
the same review. Documented in-comment that the block loop is already generic and which
invariants were re-confirmed.

**c. Configurable cap** — `Kconfig` + `include/mmwlan.h` (from earlier #6 work, range now widened):
```
config HALOW_AP_MAX_STAS   int   default 20   range 1 127
```
```c
#if defined(CONFIG_HALOW_AP_MAX_STAS)
#define MMWLAN_AP_MAX_STAS_LIMIT (CONFIG_HALOW_AP_MAX_STAS)
#else
#define MMWLAN_AP_MAX_STAS_LIMIT (20)
#endif
```
`UMAC_TWT_NUM_AGREEMENTS = MMWLAN_AP_MAX_STAS_LIMIT`, so the per-vif TWT agreement table
scales with the cap.

**d. Per-STA state strictly in PSRAM (configurable)** — `Kconfig` +
`src/umac/data/umac_data.c`:
```
config HALOW_STA_DATA_IN_PSRAM   bool   default n   # requires CONFIG_SPIRAM
```
```c
struct umac_sta_data *umac_sta_data_alloc(struct umac_data *umacd)
{
#if defined(CONFIG_HALOW_STA_DATA_IN_PSRAM) && CONFIG_HALOW_STA_DATA_IN_PSRAM
    struct umac_sta_data *stad =
        (struct umac_sta_data *)heap_caps_calloc(1, sizeof(*stad), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    struct umac_sta_data *stad = (struct umac_sta_data *)mmosal_calloc(1, sizeof(*stad));
#endif
    if (stad != NULL) { stad->umacd = umacd; }
    return stad;
}
```
**Strictly PSRAM — no internal-SRAM fallback** (a deliberate choice: at high STA counts we
want a hard failure if PSRAM is exhausted rather than silently eating the scarce internal
pool). `sdkconfig.h` is included guarded by `ESP_PLATFORM` so the upstream host build is
untouched.

**e. Reference AP app exercises the ceiling** — `firmware/rimba-halow-ap/sdkconfig.defaults`
(superproject): `CONFIG_HALOW_AP_MAX_STAS=100`, `CONFIG_HALOW_STA_DATA_IN_PSRAM=y`.
Note the *runtime* accept limit (`cfg.ap.max_stas` / `LINK_MAX_STAS` in `app_main.c`) is a
separate, smaller value tuned for the 3-board bench; the 100 here is the *build ceiling*.

---

## 3. Build verification

`make build APP=rimba-halow-ap BOARD=proto1-fgh100m` from a fresh sdkconfig.

Resolved config:
```
CONFIG_SPIRAM=y
CONFIG_HALOW_AP_MAX_STAS=100
CONFIG_HALOW_STA_DATA_IN_PSRAM=y
```
Result: **Project build complete** (exit 0), `rimba_halow_ap.bin` ~1.48 MB. All four
static asserts that guard the TIM math passed (they would have failed the compile if the
two-block arithmetic were wrong):
`MAX_SUPPORTED_PVB_LEN == 20`, `MMWLAN_AP_MAX_STAS_LIMIT < MAX_SUPPORTED_AID` (100<128),
`bitmap*8 >= MAX_SUPPORTED_AID` (128≥128), `MAX_SUPPORTED_AID < 512`.

Earlier in #6 the cap=63 + PSRAM path was also build-verified; the default (cap=20, PSRAM
off) path builds too.

---

## 4. What is NOT validated / deferred

- **High-STA hardware test.** We have 3 boards; associating ~100 real STAs to one AP is
  not reproducible on the bench. The ceiling is proven *structurally* (encoding + storage
  + asserts), not by 100 live associations. A multi-block TIM should be sanity-checked on
  air with ≥2 STAs whose AIDs straddle the 64 boundary (i.e. force an AID ≥ 64) so a STA
  in **block 1** is exercised — that is the one new code path. Needs forcing AID
  assignment past 63, which the dense allocator won't do until 64+ STAs associate.
- **Per-STA PSRAM cost at scale.** ~912 B × 100 ≈ 90 KB from PSRAM is comfortable in 8 MB,
  plus the TWT table (§6) and per-STA TX/RX buffers; total high-water not measured.
- **Interop:** Linux morse_driver as the STA parsing our two-block TIM not yet tested on air.

---

## 5. How many STAs *can* an MM6108 AP hold? (spec vs. Linux vs. firmware)

Three different ceilings, often conflated:

| Ceiling | Value | Source |
|---|---|---|
| **802.11ah spec (AID space)** | **8191** | 13-bit AID (1..8191, 0 reserved). This is the marketing number Morse Micro and the module datasheets quote ("connects up to 8,191 devices") — it is the *addressable* maximum, not a tested concurrent-association figure. |
| **Linux / Morse stack code limit** | **2007** | Found in the vendored Morse hostap + nl80211 source: `src/hostap/src/utils/morse.h: #define MAX_AID (2007)` and `driver_nl80211.c: #define RAW_CMD_MAX_AID (2007) /* set by linux */`. This is `mac80211`'s `IEEE80211_MAX_AID` — the real per-AP STA ceiling the Linux-side stack enforces (RAW groups, AID assignment). Far above anything we'd run. |
| **MM6108 firmware concurrent capacity** | **unknown / unpublished** | The closed `.mbin` keeps its own per-STA hardware context tables. No datasheet, forum, or the vendored source states a tested concurrent-STA number for the chip in AP mode. This — not our host code — is almost certainly the practical ceiling, and we cannot introspect it. |

So the honest bound for *our* ESP32 AP is: host structure now supports up to 127 (or 255 with a
larger AID space), the Linux reference stack would allow 2007, and the firmware caps it at some
unpublished number we have to discover empirically. The 8191 figure is the spec, not a promise.

Searched: morsemicro.com datasheets/product pages, module user guides, CNX-Software, Argenox,
iotclass — all repeat the 8191 spec figure; none give a tested concurrent count. The `2007`
came from reading the vendored stack source, which is more authoritative than the marketing.

---

## 6. TWT agreement table moved to PSRAM (was static internal-SRAM `.bss`)

**Finding:** `struct umac_data` is a single **file-scope static global** (`umac_data.c`), so its
embedded `struct umac_twt_data twt` — which holds `agreements[UMAC_TWT_NUM_AGREEMENTS]` +
`responder_peers[N][6]`, both sized to the STA cap — lived in **internal-SRAM `.bss`, reserved at
link time by the cap** (not by runtime STA count). The PSRAM knob from §2 only covered the
heap-allocated `umac_sta_data`, so it did **not** touch this. One packed agreement is ~48 B; with
the 6 B peer MAC that's ~54 B/STA → ~5 KB static at cap=100, ~14 KB at cap=255 — all internal SRAM.

**Change** (`umac_data.c`): converted `twt` from an embedded member to a **heap pointer** and
allocate it in `umac_data_init()`, freed in `umac_data_deinit()`, gated on the same
`CONFIG_HALOW_STA_DATA_IN_PSRAM`:
```c
struct umac_twt_data *twt;   /* was: struct umac_twt_data twt; */
...
#if CONFIG_HALOW_STA_DATA_IN_PSRAM
    umac_data.twt = heap_caps_calloc(1, sizeof(*umac_data.twt), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    umac_data.twt = mmosal_calloc(1, sizeof(*umac_data.twt));
#endif
    MMOSAL_ASSERT(umac_data.twt != NULL);
```
Safe because **every** access goes through the single accessor `umac_data_get_twt()` (verified: no
direct `umacd->twt.` field access anywhere); it now returns the pointer. Allocated before
`root.is_initialised = true`, so the accessor's sanity check still holds. Strict PSRAM, no
fallback (matches `umac_sta_data_alloc`). Even with the knob *off*, the table now comes from the
internal heap rather than `.bss`, so a large cap no longer bloats the static image.

Build-verified both ways: AP app at cap=100 + PSRAM=y (exit 0) and a non-AP app
(`rimba-halow-scan`, knob unset → `#else` path, exit 0) — this file compiles into every app, so
both branches matter.

Updated the `HALOW_STA_DATA_IN_PSRAM` Kconfig help to state it now covers both `umac_sta_data`
*and* the TWT table.
