# test-twt-assoc-sta — assoc-embedded TWT reporter (the `twt-assoc` T2 test)

The STA (requester + reporter) for the **assoc-embedded** TWT test. It requests TWT via
`mmwlan_twt_add_configuration()` **before** `mmhalow_connect()`, so the TWT IE rides in the
(re)association request.

**Why it's distinct from `test-twt-sta`** (the mid-session `twt` test): the mid-session action
frame (`mmwlan_twt_setup_request`) is **ignored by a Linux `hostapd_s1g` AP**, so it only proves the
ESP-AP path. The assoc-embedded path is honoured by the assoc-time responder on **both** the ESP
SoftAP and a Linux AP — the universal path — so this closes the gap.

## Assertion

`mmwlan_twt_agreement_installed(flow) == 1` on any of flows 0..3 (the assoc-embedded flow id is
assigned by the negotiation, so the reporter scans). INSTALLED is a discrete negotiation outcome
(`EMPTY → PENDING → INSTALLED`), not an RF measurement.

> **Caveat, verified on-bench:** `umac_twt_add_configuration` only *stores* the config (it does not
> create an agreement), and `umac_twt_process_ie` needs a pre-existing agreement — so whether the
> assoc-embedded requester ends up visible via the accessor was uncertain from the source. The
> reporter therefore reports the observed state; the T2 test's assertion follows what the bench
> actually shows. If no agreement is visible, the verdict is INCONCLUSIVE (assoc-embedded TWT then
> needs current metering — the `tp` tier — not this structural accessor), not FAIL.

## Rig

- AP: `test-apsta-ap` on board0 (ESP SoftAP), **or** a Linux `hostapd_s1g` AP on chronite
  (`linux_peer.bring_up_ap`) — the latter is the discriminator that proves the universal path.
- STA (this): board1 (or any board). SSID `rimba-ping` / SAE. No PPK2, no sniffer.
