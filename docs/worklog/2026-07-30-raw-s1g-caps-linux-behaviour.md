# 2026-07-30 — What Linux does with the S1G-caps RAW bit, and two design-doc errors it exposed

**Goal.** Before flipping the ESP's `if (false)` stub that would advertise RAW Operation Support, find out
what Linux actually does with that bit — because the porting rule says derive from Linux, and because the
next planned step (S0b-3) was written as a one-line flip to `if (true)`.

**Outcome.** The bit is **live state, not a capability advertisement**: Linux sets it once per vif from the
firmware manifest and then re-clears it on every beacon unless RAW is actually running. So **S0b-3 as
planned was wrong** — an unconditional flip would advertise RAW on an AP with no schedule configured, which
is the one thing the design doc explicitly rules out. Two further design-doc errors fell out, one of which
had a *comment* standing in for code.

**No radios were used.** Source reading plus two captures already on disk. Nothing committed.

---

## 1. The cheap answer first: an on-air A/B that was already sitting in the scratchpad

Both S0b-1 (RAW disabled) and S0b-2 (RAW enabled) captured the same AP in the same session. The S1G
Capabilities element (EID 217) from each:

```
RAW disabled : 9E 00 40 F8 80 0C 00 02 40 00 FD 00 FA 01 00    2220 beacons
RAW enabled  : 9E 00 40 F8 80 0C 08 02 40 00 FD 00 FA 01 00     236 beacons
                                 ^^
                          octet[6] bit 3 = RAW Operation Support
```

**One bit differs across the whole 15-byte element.** Same AP, same session, everything else byte-identical.

So Linux does **not** statically advertise what the chip can do — it advertises what the AP is *currently
doing*. That is a behavioural fact, obtained for free from data already captured, before reading a line of
source.

## 2. The mechanism, verified in chronite's 1.17.8 tree

```c
mac.c:1228-1229   if (MORSE_CAPAB_SUPPORTED(&mors_vif->capabilities, RAW))
                          s1g_capab->capab_info[6] |= S1G_CAP6_RAW_OPERATION;

mac.c:1392-1393   if (!morse_raw_is_enabled(mors_vif))
                          s1g_capab->capab_info[6] &= ~S1G_CAP6_RAW_OPERATION;

raw.c:1630-1632   return mors_vif && mors_vif->ap &&
                         test_bit(RAW_STATE_ENABLED, &mors_vif->ap->raw.flags);
```

- **Set once per vif** from the firmware manifest capability, inside `morse_mac_set_s1g_capab()`
  (`mac.c:1088`), which `morse_mac_vif_init_common()` calls for **every** vif type (`mac.c:3202`).
- **Cleared on every long beacon** (`beacon.c:197`, immediately before serialisation) **and every
  management frame** (`mac.c:1905`), unless RAW is live.
- The predicate needs **both** `mors_vif->ap` — kzalloc'd only for AP-type vifs (`mac.c:3236`, `:3239`) —
  **and** `RAW_STATE_ENABLED`, which is set at exactly one place tree-wide (`raw.c:1139`, inside
  `morse_raw_enable()` at `raw.c:1137`). The config entry point rejects any non-AP vif at `raw.c:1567`.
- The insert copies the template into a **fresh allocation** (`tx_11n_to_s1g.c:85-88`), so the per-frame
  clear never damages the per-vif template.

Three consequences worth recording:

- **A Linux STA can never advertise the bit.** That makes it an invalid control for an ESP STA, which
  *does* set it (via `raw_sta_priority`). Any A/B pairing the two is not apples-to-apples.
- **The AP's real per-STA RAW input is not this bit at all** — it is the **QoS Traffic Capability element,
  EID 89**, read from the association request (`raw.c:897`, priority extracted at `:907`) — and only once
  RAW is already enabled (`raw.c:890`). So the ordering is: enable RAW, *then* the AP starts learning
  per-STA priorities.
- **Short beacons carry no EID 217 at all.** The insert lives in the non-short branch only
  (`tx_11n_to_s1g.c:922`, inside the `else` of `if (short_beacon)`), and the short-beacon ordering table
  omits the element.

## 3. What this changes: S0b-3 was wrong as planned

The plan said: flip `s1g_capabilities.c:276-280` from `if (false)` to `if (true)`, capture one beacon, diff
octet[6] bit 3.

**That would diverge from Linux.** An unconditional flip advertises RAW support on an AP with no RAW
schedule configured — precisely the *"do not ship a RAW advertisement the AP cannot enforce"* outcome the
design doc rules out in its own decision table.

The correct ESP shape is `if (raw_enabled_for_this_ap)`. Which means **S0b-3 is not a one-line flip in
isolation** — it needs whatever carries "RAW is enabled" to exist first, even as a stub. That reorders the
ESP arm: the enable-carrier comes before the advertise bit, not after.

It also upgrades a guess to a hypothesis. The recon wondered whether the STA firmware keys on this bit;
the fact that Linux bothers to gate it per-frame, rather than setting it once and forgetting, is weak
evidence that someone reads it. Still unproven — see §6.

## 4. Correcting my own framing from earlier in the session

I had presented the risk as: *"Linux deletes and regenerates the S1G Capabilities element during the
S1G conversion, so the ESP firmware plausibly does too."*

The machinery is real — `morse_dot11ah_mask_ies()` clears the EID (`dot11ah/ie.c:502` → `:520`) and
`morse_dot11ah_insert_s1g_capability()` re-inserts from `mors_vif->s1g_cap_ie` (`tx_11n_to_s1g.c:78`,
called at `:922`). But **Morse's Linux hostapd has no S1G Capabilities builder at all**: no
`src/ap/ieee802_11_s1g.c`, no `hostapd_eid_s1g_capab`, and `WLAN_EID_S1G_CAPABILITIES` appears only in its
parser. The beacon mac80211 hands down carries no EID 217, so the clear deletes nothing on the AP beacon
path.

**The driver *generates* the element; it does not *regenerate* it.** My analogy to the ESP was weaker than
I presented it.

## 5. Two design-doc errors, both source-verified

### 5a. There is no legacy→S1G beacon conversion on the ESP AP path

The design doc claimed the ESP hands the firmware a **legacy PV0 beacon that the FW converts for AP-type
vifs**, and made that the whole content of Q2. It cited `umac_mesh.c:214-218`.

**That anchor is a comment.** The code says otherwise:

```c
src/hostap/src/ap/beacon.c:2357   ext_head->frame_control =
                                      IEEE80211_FC(WLAN_FC_TYPE_EXT, WLAN_FC_STYPE_S1G_BEACON);
src/hostap/src/ap/beacon.c:2673   params->head = (u8 *) ext_head;
```

hostapd builds a **finished S1G beacon**. And there is no conversion layer anywhere in the submodule —
`grep -rIl -e ies_mask -e mask_ies -e 11n_to_s1g components/halow` returns **zero files**.

So Q2's original framing describes a mechanism that does not exist on the AP path, and **the
host-built-S1G-beacon fallback (the mesh pattern) is not needed for that reason** — the AP path already
does what the fallback would have provided. The residual risk is narrower and applies to *both* platforms
equally: does the MM6108 blob rewrite IEs inside a host-supplied beacon? Source cannot answer that. Only
the capture can.

### 5b. S5 is a setter, not a port

The AP-side RAW *input* path is **already compiled into the ESP build and merely dormant**, under
`CONFIG_IEEE80211AH` (present in BUILD_DEFINES) and in files that are in SRCS:

| | |
|---|---|
| RAW-aware AID allocator | `hostapd_get_raw_aid()` `ieee802_11.c:3833`, dispatched `:3949` |
| EID-89 parser | `process_qos_traffic_cap()` `ieee802_11.c:4518`, called `:5049` |
| storage | `conf->raw_enabled`, `sta->raw_priority` |

Both call sites are gated on `hapd->conf->raw_enabled`, which defaults **false** (`ap_config.c:190`) and
whose only setters live in `hostapd/config_file.c` — **not compiled for ESP**.

So S5 needs a setter reachable from `mmwlan_ap_args`, not a from-scratch port. **This is a different layer
from the genuinely dead `hostapd_send_raw_config()`**, which is `#ifdef`'d out behind the absent
`CONFIG_DRIVER_NL80211_MORSE` — the two were being conflated, and only the latter is a dead end.

The AID-width mismatch recorded earlier still applies: hostapd's groups are 256 AIDs wide up to 2007 while
morselib caps at 255, so `hostapd_get_raw_aid` cannot be reused as-is.

## 6. Caveats

- **The blob was not searched.** `S1G_CAP6_RAW_OPERATION` has exactly three hits across the Linux driver
  (define `s1g_ies.h:64`, set, clear), zero in hostap, and morselib's getter (`dot11.h:1745`) has zero
  callers — so **no source** reads it. That is not the same as "nothing reads it": the MM6108 firmware is
  opaque, and a third-party STA could read it. Calling the flip zero-risk would be inference dressed as
  fact.
- **"A Linux STA can never advertise it" is bounded by the paths searched.** `morse_raw_enable()` is also
  reachable from `ocs.c:60` with no vif-type check. The conclusion holds for the normal configuration path
  but is not an absolute.
- **Whether the ESP firmware rewrites IEs in a host-supplied beacon remains unknown**, and no amount of
  source reading will settle it. §5a narrows the question; it does not answer it.
- This was source + existing captures only. Nothing was measured on air today beyond re-reading two
  pcaps already on disk.

## 7. Method note — the verifiers earned their keep

Per the porting rule (*"verify every cited file:line, never cite from memory; lines drift, and a code map
written from memory is reliably wrong"*), the three source-mapping passes were followed by two adversarial
verifiers whose only job was to re-open every anchor. They found:

- **Line numbers cited from chronite's WORKING TREE under a 1.17.8 tag label.** That tree carries an
  uncommitted +5-line debug hunk in `command.c`, so every cited line in that file was +5 against the tag.
  This is exactly the failure mode the rule warns about, and it would have been invisible without the
  re-check.
- **Two inverted attributions** — `beacon.c:998` labelled the beacon builder when it is inside
  `hostapd_probe_resp_fill_elems()` (the probe-response path); the beacon tail is `:2565`.
- **A blanket "all cited files are byte-identical between 1.17.8 and 1.17.9"** that was false for
  `dot11ah/main.c` — the one cited file that actually differs.
- **Several "REFUTED" conclusions** that the evidence only *narrowed*, each contradicted by the same
  report's own unknowns section.

I re-read every load-bearing anchor myself before writing any of it into the design doc.

## 8. Next

S0b-3 needs re-planning before it runs — the enable-carrier first, then the conditional advertise bit.
Worth doing alongside S0b-4 rather than as a standalone step now that it is no longer a one-liner.

The throughput A/B is unaffected by any of this and remains the cheapest remaining item that needs no ESP
code.
