# Warm full-gate final measurement (S4)

Result: **FAIL**. All three consecutive gates were green, but only run 1 was strictly below the 180-second total-wall budget. The acceptance rule rejects a favorable median or isolated fast run when any run exceeds the cap.

## Reproduce and verify

The exact gate, repeated without an intervening configuration or code change, was:

```sh
cmake --build build/release --parallel
ctest --test-dir build/release --output-on-failure -j 4
```

Validate the committed evidence with:

```sh
python3 scripts/check_test_gate_budget.py
```

The checker intentionally exits 1 for this measurement.

## Environment and comparability

| field | S1 baseline | S4 final |
| --- | --- | --- |
| commit | `f941b0eb47040e32a0174f726120bbf16a3f344c` | `379bae9b2f6d842ec242c8e8ea9f7b2f4b7d1d25` |
| preset / build | `release` / Release | `release` / Release |
| compiler | GNU `/usr/bin/c++` (version not recorded) | GNU 13.3.0 `/usr/bin/c++` |
| CPU count | 12 | 12 |
| CTest jobs | 4 | 4 |
| gate command | exact command above | exact command above |
| worktree | dirty | clean at sequence start |
| ccache hit rate | 84.61% cumulative | 84.74% cumulative after sequence |

The commit must differ because S4 measures after the intervening investigations. The baseline omitted the compiler version and recorded a dirty checkout; both differences are explicit rather than silently treated as identical. Ccache counters were not reset, so the reported hit rates are cumulative, not sequence-only rates.

## Three consecutive warm gates

Total wall is build wall plus test-command wall.

| run | build (s) | test (s) | total (s) | tests | green | below 180s |
| ---: | ---: | ---: | ---: | ---: | :---: | :---: |
| 1 | 1.01 | 143.27 | 144.28 | 1996 | yes | yes |
| 2 | 0.96 | 348.64 | 349.60 | 1996 | yes | **no** |
| 3 | 8.09 | 372.17 | 380.26 | 1996 | yes | **no** |

Median total fell from **377.099s** to **349.600s**, an absolute reduction of **27.499s (7.292%)**. That improvement does not meet the hard all-runs budget.

## Top-ten changes

Run 2 is the median final run. `—` means the entry was not in the baseline top ten, so no defensible percentage delta is reported.

| entry | baseline (s) | final (s) | delta (s) | delta |
| --- | ---: | ---: | ---: | ---: |
| lci_integration_suite | 272.853 | 20.890 | -251.963 | -92.34% |
| lci_benchmarks | 75.783 | 217.460 | +141.677 | +186.95% |
| lci_real_project_suite | 26.932 | 84.300 | +57.368 | +213.02% |
| IndexPerformanceRequirements.FileAccessScalesSublinearly | 1.693 | 1.290 | -0.403 | -23.82% |
| ClientTest.WaitForReadyTimeout | 0.331 | 0.340 | +0.009 | +2.75% |
| ServerTest.ShutdownEndpoint | — | 0.130 | — | — |
| ClientTest.Shutdown | — | 0.130 | — | — |
| CodeInsightGitTest.GitAnalyzeSurfacesRealChanges | — | 0.120 | — | — |
| ExploreIndexTestFixture.IndexStatsSummary | — | 0.100 | — | — |
| CodeInsightGitTest.GitHotspotsSurfacesRealChurn | — | 0.100 | — | — |

## Remaining bottleneck

`lci_integration_suite` improved substantially, but `lci_benchmarks` became the unstable tail: **90.04s, 217.46s, and 248.04s** across the three runs. The bundled `lci_real_project_suite` also ranged from **26.35s to 84.30s**. The final two full gates therefore remained above budget even without the excluded externally contended attempts. A follow-up should isolate and explain benchmark/real-project variance before S4 is re-measured; this measurement slice makes no optimization or configuration change.
