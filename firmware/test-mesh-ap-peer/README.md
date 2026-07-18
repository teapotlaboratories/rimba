# test-mesh-ap-peer — far mesh node of the mesh-ap T2 test

The far mesh node (board1, 10.9.9.100) the STA pings through the gate. Derived from `rimba-halow-mesh`
with its return route repointed to the **board2 gate** (`10.9.9.108`) so off-subnet echo replies to
the STA route back via the gate. Support role (up-marker `mesh-peer-up`).

Full test: [`../test-mesh-ap/README.md`](../test-mesh-ap/README.md).
