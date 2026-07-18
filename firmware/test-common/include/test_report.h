/* test_report.h — the console contract between a test-* app and the harness.
 *
 * Every firmware/test-<feature>/ app reports its verdict on the serial console in a
 * fixed, greppable format. tools/regtest/t2_onair.py scrapes these lines; a human reads
 * the same lines. One format, both audiences — so an agent and a person never disagree
 * about whether a test passed.
 *
 * The contract:
 *
 *   TEST|BEGIN|name=<slug>|rig=<what this needs>
 *   TEST|INFO|<free text>                     -- progress, values, observations
 *   TEST|STEP|<step>|<PASS|FAIL>|<detail>     -- a sub-check
 *   TEST|RESULT|<PASS|FAIL|INCONCLUSIVE>|<detail>
 *   TEST|END|name=<slug>
 *
 * Rules that make the result trustworthy:
 *
 *  - Exactly ONE RESULT line per run. The harness fails a run that emits none (a board
 *    that hung or crashed must not read as a pass by silence).
 *  - INCONCLUSIVE is a first-class verdict, not a cop-out. Use it when the test could
 *    not obtain a trustworthy measurement — no peer appeared, the RF link was
 *    overloaded, the rig was wrong. A bench artifact reported as FAIL is worse than
 *    useless: it trains everyone to ignore the suite.
 *  - Print the OBSERVED value in the detail, always, even on PASS. A bare "PASS" cannot
 *    be audited; "PASS peers=1 plink=ESTAB rssi=-58" can.
 */

#ifndef TEST_REPORT_H
#define TEST_REPORT_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Printed directly rather than through ESP_LOGI so the lines carry no log-level
 * prefix, colour codes, or tag — the harness regex stays trivial and stable, and the
 * lines survive a log-level change. */
#define TEST_LINE(...) do { printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

#define TEST_BEGIN(name, rig)  TEST_LINE("TEST|BEGIN|name=%s|rig=%s", (name), (rig))
#define TEST_END(name)         TEST_LINE("TEST|END|name=%s", (name))

#define TEST_INFO(fmt, ...)    TEST_LINE("TEST|INFO|" fmt, ##__VA_ARGS__)

#define TEST_STEP(step, ok, fmt, ...) \
    TEST_LINE("TEST|STEP|%s|%s|" fmt, (step), (ok) ? "PASS" : "FAIL", ##__VA_ARGS__)

#define TEST_PASS(fmt, ...)    TEST_LINE("TEST|RESULT|PASS|" fmt, ##__VA_ARGS__)
#define TEST_FAIL(fmt, ...)    TEST_LINE("TEST|RESULT|FAIL|" fmt, ##__VA_ARGS__)

/* Could not obtain a trustworthy measurement. NOT a failure of the code under test. */
#define TEST_INCONCLUSIVE(fmt, ...) \
    TEST_LINE("TEST|RESULT|INCONCLUSIVE|" fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* TEST_REPORT_H */
