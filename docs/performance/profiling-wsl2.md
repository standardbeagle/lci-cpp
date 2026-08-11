# Profiling on WSL2 — verified setup and recipes

Everything below was verified on this machine on 2026-08-11:
kernel `6.18.33.2-microsoft-standard-WSL2`, Ubuntu 24.04 (noble),
Intel i7-8750H (Skylake), Store-distributed WSL.

## The headline: hardware PMU counters WORK here

The folklore that "WSL2 has no PMU, software events only" is stale for
this setup. `/sys/bus/event_source/devices/cpu` is present and perf
counts real hardware events:

```
perf stat -e cycles,instructions,cache-misses,branch-misses -- <cmd>
# 514,635 cycles / 165,692 instructions / 10,445 cache-misses (verified)
```

Recent WSL2 kernels + the Store WSL distribution pass the Intel PMU
through. If this ever regresses after a WSL update, re-run the command
above; "not supported" for `cycles` means the passthrough is gone and
only `-e task-clock,page-faults,context-switches` (software events)
remain trustworthy.

## One-time setup (already applied on this machine)

1. **perf binary.** `linux-tools-generic` ships perf under a kernel-
   versioned dir the WSL kernel name never matches. The wrapper is
   satisfied with a symlink — perf userspace does not actually care:

   ```sh
   sudo mkdir -p /usr/lib/linux-tools/$(uname -r)
   sudo ln -sf /usr/lib/linux-tools-6.8.0-137/perf \
       /usr/lib/linux-tools/$(uname -r)/perf
   ```

   After a kernel *or* linux-tools upgrade the link needs refreshing
   (new `uname -r` / new tools dir).

2. **sysctls** — persisted in `/etc/sysctl.d/60-profiling.conf`:

   ```
   kernel.perf_event_paranoid = 1   # user profiling incl. kernel-side samples
   kernel.kptr_restrict = 0         # kernel symbol resolution in reports
   kernel.perf_event_mlock_kb = 2048
   ```

3. **Tools:** `heaptrack` 1.5.0, `valgrind` 3.22 (massif/DHAT), and
   perf 6.8.12 (version skew vs the 6.18 kernel is fine for the
   features used here).

4. **Profilable binaries:** the `profile` CMake preset =
   Release codegen + `-g` (DWARF: inline/name resolution, zero runtime
   cost) + `-fno-omit-frame-pointer` (~1% cost, cheap unwinding).
   Builds into `build/profile`. The plain `release` preset stays
   pristine so perf baselines stay comparable.

   ```sh
   cmake --preset profile && cmake --build build/profile --parallel --target lci
   ```

## Recipes (use build/profile binaries)

### CPU: where do cycles go?
```sh
perf record -g --call-graph fp -F 999 -- \
    ./build/profile/src/lci -r <corpus> debug memprofile --top 1
perf report --stdio | head -50        # or: perf report (TUI)
```
`--call-graph dwarf,16384` gives better stacks through callbacks at
~10x the perf.data size; fp is the default choice now that frame
pointers are compiled in.

### Memory: who allocated what (per callsite)?
```sh
heaptrack ./build/profile/src/lci -r <corpus> debug memprofile --top 1
heaptrack_print heaptrack.lci.*.gz | tail -60   # peak contributions
heaptrack_print -p 20 ...                        # top 20 peak callsites
```
This is the tool that would have found the postings token-string term
and the scope-chain duplication in one run. Prefer it over extending
the in-house census for *discovery*; keep `lci debug memprofile` for
deterministic regression numbers (its census is CI-able, heaptrack
output is not).

### Allocation churn (temporary allocations, not retention)
```sh
heaptrack_print -T ...        # temporary allocations ranking
valgrind --tool=dhat ./build/profile/src/lci ...   # exact but ~20x slow
```

### Proven working end-to-end (2026-08-11, this repo)
- `perf record -g --call-graph fp -F 499` over an lci index build:
  full named stacks main -> CLI -> run_debug_memprofile, 90%+
  attribution.
- `heaptrack` over an index build: per-callsite peaks with file:line,
  first finding = absl raw_hash_set resize as top peak consumer (the
  same container-growth class the census caveats).

### What still does NOT work on WSL2 (each FAILED live, same day)
- `perf record -e cycles:ppp` -- "PMU Hardware doesn't support
  sampling/overflow-interrupts" in PEBS-precise mode. Plain sampling
  works; precise skid-free attribution does not.
- `perf mem` / `perf c2c` -- "memory events not supported." No
  false-sharing analysis on WSL2; this matters for the lock-free RCU
  read paths and is the #1 reason to keep a bare-metal box: run
  `perf c2c record` there under concurrent search load.
- Uncore/offcore events (memory bandwidth), VTune.
- `perf record` of **another already-running process you don't own** at
  paranoid=1 (use `sudo perf` or attach as the same user).
- OOM forensics: the WSL2 VM can be resized/ballooned by Windows;
  `dmesg` survives within a VM lifetime but a WSL restart wipes it.
  For OOM hunts, cap the process (`systemd-run --scope -p MemoryMax=`)
  so the kill is observable, or watch `/proc/<pid>/status` live.
- Anything relying on persistent daemons across Windows reboots — WSL
  VMs are ephemeral; sysctls above are re-applied by systemd, the perf
  symlink survives (it lives in the distro filesystem).

## Cross-check discipline

RSS on WSL2 sits on a VM whose balloon can distort absolute numbers
under Windows memory pressure. For memory *ratios* (the ≤2x budget)
this washes out; for absolute regressions, prefer the census +
mallinfo2 live-alloc numbers from `lci debug memprofile`, which are
VM-independent.
