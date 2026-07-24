# test-mesh-gate-node

The **mesh-node reporter** for the T2 `mesh-gate-b2` test (Case D): a mesh node that pings a **silent,
static-IP AP client** (`10.9.9.50`, `NO_PING`) behind the retire-L3 gate — the mesh→AP direction (B2).

Rig (3 boards): gate = `rimba-halow-mesh-ap` (board2) · sta = `rimba-halow-sta STA_IP=10.9.9.50 NO_PING=1`
(board0, static silent responder) · this mesh node = board1 (reporter).

Because the STA is silent, the node must resolve it **cold** across the bridge: its ARP is a mesh broadcast
the gate bridges down to the AP (B2), which is No-Ack **lossy**. Reliability comes from the gate's proxy-ARP
proactive push, not the broadcast. So **PASS** iff ≥15/30 replies (broken ≈ 0–3; proxy-ARP-reliable ≈ 25–30;
a wide floor tolerates the lossy warmup) — this is the direction that was 0-reply flaky before proxy-ARP.

Run: `make test-t2 TEST=mesh-gate-b2 ...` (needs `tools/ppk2_hold.py` for board2).
