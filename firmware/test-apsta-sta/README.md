# test-apsta-sta — STA reporter for the AP↔STA ping test

This is the **STA (reporter) role** of the `ap-sta-ping` T2 test, not a standalone test. It
associates to the `rimba-halow-ap` SoftAP, pins a static IP, pings the AP, and emits the
`TEST|` verdict the orchestrator scrapes.

**Full test docs, rig, and how to run:** [`../test-ap-sta-ping/README.md`](../test-ap-sta-ping/README.md).

Run it via the orchestrator (do not flash by hand unless debugging):

```sh
make test-t2 TEST=ap-sta-ping
```
