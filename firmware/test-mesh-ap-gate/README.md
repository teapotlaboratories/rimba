# test-mesh-ap-gate — GATE role of the mesh-ap T2 test

The mesh-gate support role: mesh (primary vif) + SoftAP (secondary vif) + `ip_forward` on one
MM6108. Derived from the product `rimba-halow-mesh-ap` app + a `TEST|` up-marker (`gate-ready`).
Runs on **board2** (require_wired). Not a standalone test — the STA reporter emits the verdict.

Needs `CONFIG_HALOW_AP_MODE=y` + `CONFIG_LWIP_IP_FORWARD=y`. Full test:
[`../test-mesh-ap/README.md`](../test-mesh-ap/README.md). Run: `make test-t2 TEST=mesh-ap`.
