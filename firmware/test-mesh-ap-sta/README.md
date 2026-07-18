# test-mesh-ap-sta — STA reporter of the mesh-ap T2 test

Behind the gate's AP (board0), associates, then pings the far mesh node `10.9.9.100` and asserts
**ttl=63** — the gate's single `ip4_forward` decrement proves exactly one gate hop between the AP
subnet and the mesh subnet. Emits the `TEST|RESULT` verdict.

Full test: [`../test-mesh-ap/README.md`](../test-mesh-ap/README.md).
