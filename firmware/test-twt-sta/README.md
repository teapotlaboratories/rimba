# test-twt-sta — STA requester+reporter for the TWT test

The **STA (requester + reporter) role** of the `twt` T2 test, not a standalone test. Associates to
`test-apsta-ap`, sends a TWT Setup Request, and reports whether the agreement reached INSTALLED.

**Full test docs:** [`../test-twt/README.md`](../test-twt/README.md). Run via:

```sh
python tools/regtest/run.py t2 --test twt
```
