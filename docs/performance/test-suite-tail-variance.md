# Bundled test tail variance (S4a)

The measured cause is missing scheduler isolation for `lci_benchmarks`. Both
bundled entries repeatedly construct `MasterIndex` instances across real
corpora, use roughly three CPU cores, and peak above 2 GiB RSS. CTest already
serialized `lci_real_project_suite`; it did not serialize `lci_benchmarks`.

## Reproduction

All entry runs used commit `1330bca900b50668a96c03e81d79803d12f8b90e`,
the `release` preset (GNU 13.3.0), the same repository `real_projects` corpus,
and warm build/filesystem caches. Host load, CPU time, wall time, and RSS were
recorded for every run.

| entry | before wall seconds (5 consecutive) | after wall seconds (5 consecutive) | after max/min |
| --- | --- | --- | ---: |
| `lci_benchmarks` | 102.80, 128.22, 120.74, 105.59, 111.66 | 91.48, 95.16, 146.96, 91.27, 104.23 | 1.607 |
| `lci_real_project_suite` | 27.88, 29.09, 30.76, 27.55, 27.27 | 36.86, 49.20, 39.80, 47.82, 37.15 | 1.335 |

The benchmark consumed 250–298% CPU and 2.12–2.18 GiB RSS. The real-project
suite consumed 252–317% CPU and 2.54–2.61 GiB RSS. Higher host load coincided
with lower process CPU efficiency and longer walls. This directly localizes
the variance to contention around the internally parallel bundled processes,
not a conclusion inferred from the full-suite totals.

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

Three green confirmations could not be collected in the current shared host
state. The first attempt measured the corrected tails at 124.69s and 34.25s,
then `lci_integration_suite` timed out at 600s after socket bind failures. A
second attempt had server tests fail `server.start()` immediately. Read-only
process inspection identified two pre-existing long-lived `lci` servers; the
task initially did not authorize terminating them. After those exact orphans
were removed, a clean-window retry built in 1.00s, ran the unit phase, and then
again had every integration `server.start()` fail before the integration bundle
timed out at 600.05s. That clean-start result demonstrates a distinct
server-process/socket isolation defect inside the exact release gate rather
than residual pre-run host state. These attempts are invalid evidence, not
confirmations. Zero green confirmations are claimed. This slice makes no
180-second policy or baseline decision.
