# test-apsta-ap — AP support role (SoftAP)

The **AP support role** shared by the `ap-sta-ping` and `twt` T2 tests. Brings up an 802.11ah
SoftAP (SSID `rimba-ping`, SAE, PMF), pins static IP 192.168.12.1, answers ICMP, and prints a
`TEST|INFO|ap-ready` up-marker. An AP vif is a **TWT responder by default**, so the same app
serves both tests. Not a standalone test (no verdict — the STA reporter emits it).

Needs `CONFIG_HALOW_AP_MODE=y` (its `sdkconfig.defaults`). Full docs:
[`../test-ap-sta-ping/README.md`](../test-ap-sta-ping/README.md).
