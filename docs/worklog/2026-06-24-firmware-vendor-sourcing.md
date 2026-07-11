# Worklog — 2026-06-24 — Firmware single-sourced from vendor/morse-firmware

**Author:** Aldwin
**Goal:** stop vendoring pre-converted firmware blobs in a fork; make
`vendor/morse-firmware` (upstream ELF) the single source of truth, with the ESP32
`.mbin` produced at build time. Remove `components/firmware` entirely.
**Status:** ✅ done + HW-verified (AP+STA boot **1.17.9**, associate, ping).

## Before

Two firmware sources, out of sync:
- ESP32 flashed from the **`components/firmware` submodule** (the `mm-esp32-firmware`
  fork), which committed **pre-converted `.mbin`** blobs at **1.17.6**.
- `vendor/morse-firmware` separately tracked the upstream **ELF `.bin`** at 1.17.9.

The fork had to be hand-maintained on every firmware change.

## After

`vendor/morse-firmware` is the only firmware in the repo. Nothing firmware-related
is committed under `components/`. The firmware "component" is a **template** that is
**copied into the build tree** at configure time and converts the ELF there.

```
cmake/mm-fw-gen/
├── morse_firmware.cmake          # helper apps include; copies the template, adds to EXTRA_COMPONENT_DIRS
└── firmware/                     # the `firmware` component template (real files)
    ├── CMakeLists.txt            # ELF -> .mbin (convert-bin-to-mbin.py) -> .o (objcopy) -> link
    ├── Kconfig                   # MM_FW_FILE / MM_BCF_FILE / MMHAL_CHIP_TYPE_*
    └── idf_component.yml         # version 2.10.4-esp32-2 (satisfies halow's morsemicro/firmware dep)
```

Each radio app (ap/sta/ibss/scan), before `project()`:
```cmake
include(".../cmake/mm-fw-gen/morse_firmware.cmake")
morse_firmware_generate_component()    # file(COPY) template -> build/<app>/mm-fw-gen/firmware/
```

### Pipeline (all build-time, all under build/<app>)
```
vendor/morse-firmware/*.bin (ELF, committed, single source of truth)
   │ convert-bin-to-mbin.py
   ▼
build/<app>/.../mm-fw-gen/firmware/mm6108.mbin   (Morse TLV)
   │ objcopy --redefine-sym -> firmware_binary_* / bcf_binary_*, --rename-section .rodata._fw_mbin
   ▼
…mm6108.mbin.o   ── target_link_libraries ──▶ flashed app image
```

The `.mbin -> .o` objcopy step is **verbatim from the upstream component**
(`morsemicro/firmware 2.10.4-esp32-2`); only the `.bin -> .mbin` step is new. The
exact symbols matter — morselib's `mmhal_wlan_binaries.c` references
`firmware_binary_start/_end` + `bcf_binary_start/_end`; IDF's native binary-embed
would emit `_binary_<filename>_*` names instead, hence the manual `objcopy --redefine-sym`.

## Why a copied template (not a checked-in component, not file(WRITE))

- **Not `components/firmware`:** the template lives under `cmake/`, which is *not* a
  component search dir, so IDF never sees it as a live component — it only becomes one
  once copied under `build/<app>`. So `components/` holds only `halow`.
- **Copied real files, not generated strings:** the component `CMakeLists.txt` is a
  normal readable/diffable file, not embedded as a string in the helper.
- **Runs at the component's top level:** so `idf_component_register()` works directly.
  (An earlier attempt called it from inside a CMake *function*, which swallowed the
  registration — IDF reported "component 'firmware' … not registered". A `macro`
  fixed that, but the copied-real-file approach avoids the issue entirely.)
- **No halow change:** the copied component is named `firmware` and versioned
  `2.10.4-esp32-2`, so halow's `morsemicro/firmware` dependency resolves to it locally.

## Gotchas / notes

- Repo paths in the copied component are resolved from `CMAKE_SOURCE_DIR`
  (= `firmware/<app>`, repo root two up) — it can't use its own
  `CMAKE_CURRENT_LIST_DIR` (that's the build copy's dir).
- Build now needs Python + **pyelftools** (already in the IDF env) to run the converter.
- Firmware version follows `vendor/morse-firmware`'s pin — currently **1.17.9**. Bump
  the firmware by bumping that submodule.
- `rimba-halow-mesh` (other branch) and `rimba-hello` (radio-free) are not wired:
  mesh needs the same two lines when its branch merges; hello has no radio.
- `dependencies.lock` is gitignored, so the build-specific generated path it records
  is not committed.

## Verification

`rimba-halow-sta` + `rimba-halow-ap` clean-built, both flashed: STA banner
`Morse firmware version: 1.17.9`, SAE association OK, AP↔STA ping ~12–37 ms. Evidence:
`/tmp/fw-copy-sta.log`.
