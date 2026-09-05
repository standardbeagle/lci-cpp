# Bundled test tail variance (S4a)

Both bundled entries repeatedly construct `MasterIndex` instances across real
corpora, use roughly three CPU cores, and peak above 2 GiB RSS. CTest already
serialized `lci_real_project_suite`; it did not serialize `lci_benchmarks`.
Declaring `lci_benchmarks` `RUN_SERIAL` is necessary hygiene: under `-j 4` CTest
could overlap it with other tests. It is not shown to be sufficient. The
before/after series below ran each entry alone (`-R '^<entry>$'`), so neither
series contained any CTest overlap, and the after series is not faster or
tighter than the before series. What the numbers demonstrate is that both tails
are sensitive to host contention from processes outside CTest; the residual
variance is a host-load effect that scheduler isolation inside CTest cannot
remove. (Corrected 2026-09-05 at review; the original text claimed missing
scheduler isolation as the measured cause. Corroboration on 2026-09-05: three
exact full gates on the same tree spanned 354s, 840s, 545s tracking host load
3, 32, 20 from other sessions.)

## Reproduction

All entry runs used commit `1330bca900b50668a96c03e81d79803d12f8b90e`,
the `release` preset (GNU 13.3.0), the same repository `real_projects` corpus,
and warm build/filesystem caches. Wall time is recorded per run below. CPU
time and RSS were observed as ranges only; host load was observed but not
recorded numerically, so the load/wall correlation stated below cannot be
re-derived from this record (tracked as a follow-up).

| entry | before wall seconds (5 consecutive) | after wall seconds (5 consecutive) | after max/min |
| --- | --- | --- | ---: |
| `lci_benchmarks` | 102.80, 128.22, 120.74, 105.59, 111.66 | 91.48, 95.16, 146.96, 91.27, 104.23 | 1.607 |
| `lci_real_project_suite` | 27.88, 29.09, 30.76, 27.55, 27.27 | 36.86, 49.20, 39.80, 47.82, 37.15 | 1.335 |

The benchmark consumed 250–298% CPU and 2.12–2.18 GiB RSS. The real-project
suite consumed 252–317% CPU and 2.54–2.61 GiB RSS. Higher host load coincided
with lower process CPU efficiency and longer walls. This localizes the
variance to CPU contention around the internally parallel bundled processes
rather than to anything inferred from full-suite totals; it does not show that
CTest overlap was the contending load, because each series ran the entry alone.

## Regression and fix

The focused `tail_bundle_isolation_contract` regression was added first and
failed because generated CTest metadata omitted `RUN_SERIAL` for
`lci_benchmarks`. The minimal fix adds that property. The regression then
passed. No test, benchmark, label, or coverage was removed or skipped.

## Commands

```sh
cmake --preset release
ctest --test-dir build/release -R '^tail_bundle_isolation_contract$' --output-on-failure
ctest --test-dir build/release --output-on-failure -R '^lci_benchmarks$' -j 1
ctest --test-dir build/release --output-on-failure -R '^lci_real_project_suite$' -j 1
cmake --build build/release --parallel
ctest --test-dir build/release --output-on-failure -j 4
```

## Full release confirmation status

Three consecutive exact gates passed on
`2df9da5103914355d85a2d9497250d2b85d79afb` after the separately tracked socket
lifecycle fix. No orphan cleanup was required during the sequence.

| run | build (s) | test (s) | total (s) | benchmark (s) | real project (s) | integration (s) | result |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| 1 | 31.96 | 279.63 | 311.59 | 187.26 | 62.67 | 19.57 | 1997/1997 |
| 2 | 2.43 | 269.81 | 272.24 | 185.95 | 55.19 | 17.97 | 1997/1997 |
| 3 | 2.17 | 241.02 | 243.19 | 178.33 | 45.56 | 16.32 | 1997/1997 |

Prior invalid attempts remain part of the record. The first measured the
corrected tails at 124.69s and 34.25s,
then `lci_integration_suite` timed out at 600s after socket bind failures. A
second attempt had server tests fail `server.start()` immediately. Read-only
process inspection identified two pre-existing long-lived `lci` servers; the
task initially did not authorize terminating them. After those exact orphans
were removed, a clean-window retry built in 1.00s, ran the unit phase, and then
again had every integration `server.start()` fail before the integration bundle
timed out at 600.05s. That clean-start result demonstrates a distinct
server-process/socket isolation defect inside the exact release gate rather
than residual pre-run host state. These attempts are invalid evidence, not
confirmations. The later three-run sequence above is the confirmation evidence.
This slice makes no 180-second policy or baseline decision.
