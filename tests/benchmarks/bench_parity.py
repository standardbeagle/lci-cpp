#!/usr/bin/env python3
"""bench_parity.py — compare C++ port indexing perf vs Go reference.

Times cold-cache index build for each real-project corpus through both
binaries, computes the C++/Go ratio, fails (exit 1) if any ratio
exceeds the configured threshold. Run via ctest target bench_parity
(see tests/CMakeLists.txt) or directly:

  python3 tests/benchmarks/bench_parity.py \\
      --go /home/beagle/work/core/lci/lci-linux-amd64 \\
      --cpp /home/beagle/work/core/lci-cpp/build/release/src/lci \\
      --threshold 2.0

Methodology:
  - Each binary is invoked with `status` from the corpus root. status
    auto-spawns the daemon, blocks until the daemon reports ready=true,
    then exits. The wall time from invocation to exit is the cold-
    index time (mmap + tree-sitter parse + symbol/trigram index).
  - Daemons are killed between trials so caches don't pollute results.
  - Two trials per (binary, corpus); the minimum wins.

Thresholds match Karpathy's "Go is the bar" rule. Default 2.0× allows
some headroom for divergent build pipelines; tighten as the C++ port
matures.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path


def run_once(binary: str, corpus: Path) -> float:
    """Cold-index timing: kill any leftover daemon, run `status` from
    corpus dir, measure wall to exit."""
    # Best-effort daemon kill (corpus-scoped pgrep).
    try:
        subprocess.run(
            ["pkill", "-9", "-f", f"--root {corpus.resolve()}"],
            stderr=subprocess.DEVNULL, check=False,
        )
        time.sleep(0.5)
    except FileNotFoundError:
        pass

    env = os.environ.copy()
    env["TMPDIR"] = env.get("TMPDIR", "/tmp")

    start = time.monotonic()
    res = subprocess.run(
        [binary, "status"], cwd=str(corpus), env=env,
        capture_output=True, text=True, timeout=600,
    )
    elapsed = time.monotonic() - start
    if res.returncode != 0:
        print(f"  [warn] {binary} status returned {res.returncode}: "
              f"{res.stderr[:200]}", file=sys.stderr)
    return elapsed


def best_of(binary: str, corpus: Path, trials: int) -> float:
    times = [run_once(binary, corpus) for _ in range(trials)]
    return min(times)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--go", required=True, help="Go lci binary path")
    parser.add_argument("--cpp", required=True, help="C++ lci binary path")
    parser.add_argument("--root", required=True,
                        help="lci-cpp repo root (for real_projects/)")
    parser.add_argument("--threshold", type=float, default=2.0,
                        help="Max C++/Go wall ratio (default 2.0)")
    parser.add_argument("--trials", type=int, default=2,
                        help="Runs per (binary, corpus) — min wins")
    args = parser.parse_args()

    real = Path(args.root) / "real_projects"
    if not real.is_dir():
        print(f"[skip] real_projects/ not found at {real}", file=sys.stderr)
        return 0  # Not a failure when corpora absent (CI without prep).

    for tool in (args.go, args.cpp):
        if not Path(tool).is_file() or not os.access(tool, os.X_OK):
            print(f"[skip] binary not executable: {tool}", file=sys.stderr)
            return 0

    # Enumerate available corpora (skip empty submodules).
    corpora = []
    for lang_dir in sorted(real.iterdir()):
        if not lang_dir.is_dir():
            continue
        for proj_dir in sorted(lang_dir.iterdir()):
            if not proj_dir.is_dir():
                continue
            # has at least one file
            has_files = any(p.is_file()
                            for p in proj_dir.rglob("*"))
            if has_files:
                corpora.append((lang_dir.name, proj_dir.name, proj_dir))

    if not corpora:
        print("[skip] no real-project corpora available", file=sys.stderr)
        return 0

    print(f"{'Corpus':<30}{'Go (s)':>10}{'C++ (s)':>10}"
          f"{'Ratio':>8}{'Status':>10}")
    print("-" * 68)

    fail = False
    results = []
    for lang, name, path in corpora:
        try:
            go_t = best_of(args.go, path, args.trials)
            cpp_t = best_of(args.cpp, path, args.trials)
        except subprocess.TimeoutExpired:
            print(f"{lang}/{name:<22} TIMEOUT", file=sys.stderr)
            fail = True
            continue

        ratio = cpp_t / go_t if go_t > 0 else float("inf")
        status = "PASS" if ratio <= args.threshold else "FAIL"
        if status == "FAIL":
            fail = True
        print(f"{lang+'/'+name:<30}{go_t:>10.2f}{cpp_t:>10.2f}"
              f"{ratio:>8.2f}{status:>10}")
        results.append(dict(corpus=f"{lang}/{name}", go_s=go_t,
                            cpp_s=cpp_t, ratio=ratio, status=status))

    # Persist for CI artifact upload.
    out_dir = Path(args.root) / "build" / "bench"
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "bench_parity.json").write_text(json.dumps(
        dict(threshold=args.threshold, results=results), indent=2))

    print(f"\nThreshold: {args.threshold}×")
    if fail:
        print("FAIL: at least one corpus exceeds threshold.")
        return 1
    print("PASS: all corpora within threshold.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
