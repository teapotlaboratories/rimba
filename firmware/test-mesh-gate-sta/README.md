# test-mesh-gate-sta

An AP client behind the mesh-gate, in one of two build-time modes:

- **default (reporter)** — DHCP client that pings a mesh node (`10.9.9.100`) zero-config across the flat L2
  bridge (`mesh-gate-bridge` / Case C). PASS iff associated + leased + ≥6 replies + `ttl==64` (pure L2
  bridge; a re-added `ip_forward` → 63 → caught).
- **`STA_IP=x NO_PING=1` (responder)** — a SILENT static AP-client at `x` (support role in `mesh-gate-b2` /
  Case D): the cold target a mesh node must resolve across the bridge. No `TEST|` verdict; up_marker
  `STA static IP`.

Gate = `test-mesh-gate-ap`. Run via `make test-t2 TEST=mesh-gate-bridge` / `TEST=mesh-gate-b2`.
