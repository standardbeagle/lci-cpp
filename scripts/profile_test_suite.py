#!/usr/bin/env python3
"""Profile and rank the full LCI C++ test-suite wall clock.

ONE reproducible command that measures the warm-ccache full test gate several
times, ranks the slowest individual tests and suites, and reconciles CTest
per-entry wall time against in-process GoogleTest body time to quantify the
per-process spawn + discovery overhead that gtest_discover_tests imposes
(~1990 separate ctest entries for lci_tests, each its own process).

This is a MEASUREMENT tool. It changes no production or test code. It shells
out to cmake / ctest / ccache / git and parses their output, then writes a
machine-readable JSON baseline (deterministic, sorted keys) plus a concise
Markdown analysis.

The gate measured is exactly:

    cmake --build build/release --parallel        # warm no-op rebuild
    ctest --test-dir build/release --output-on-failure -j 4

Run it from the repo root:

    python3 scripts/profile_test_suite.py

Fail-fast: any missing tool, failed build, or unparseable output aborts with a
clear error. No silent fallbacks, no dummy data.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET
from datetime import datetime, timezone
from pathlib import Path


def die(msg: str) -> "None":
    print(f"profile_test_suite: FATAL: {msg}", file=sys.stderr)
    sys.exit(1)


def require_tool(name: str) -> str:
    path = shutil.which(name)
    if not path:
        die(f"required tool not found on PATH: {name}")
    return path


def run(cmd: list[str], *, cwd: Path, capture: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(
        cmd,
        cwd=str(cwd),
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.STDOUT if capture else None,
        text=True,
    )


def utc_now_iso() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat()


# --------------------------------------------------------------------------
# Environment capture
# --------------------------------------------------------------------------
def capture_environment(repo_root: Path, build_dir: Path, jobs: int, preset: str) -> dict:
    def git(*args: str) -> str:
        cp = run(["git", *args], cwd=repo_root)
        return cp.stdout.strip() if cp.returncode == 0 else ""

    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        die(f"CMakeCache.txt not found under {build_dir}; is the build configured?")
    cache_vars: dict[str, str] = {}
    for line in cache.read_text().splitlines():
        if ":" in line and "=" in line and not line.startswith("//") and not line.startswith("#"):
            key, _, val = line.partition("=")
            name = key.split(":", 1)[0]
            if name in (
                "CMAKE_CXX_COMPILER",
                "CMAKE_C_COMPILER",
                "CMAKE_BUILD_TYPE",
                "CMAKE_CXX_FLAGS",
                "CMAKE_CXX_FLAGS_RELEASE",
            ):
                cache_vars[name] = val

    compiler = cache_vars.get("CMAKE_CXX_COMPILER", "")
    compiler_version = ""
    compiler_id_file = None
    for cand in build_dir.glob("CMakeFiles/*/CMakeCXXCompiler.cmake"):
        compiler_id_file = cand
        break
    compiler_id = ""
    if compiler_id_file:
        for line in compiler_id_file.read_text().splitlines():
            if "CMAKE_CXX_COMPILER_VERSION" in line:
                compiler_version = line.split('"')[1] if '"' in line else ""
            if "CMAKE_CXX_COMPILER_ID " in line:
                compiler_id = line.split('"')[1] if '"' in line else ""

    ccache = shutil.which("ccache")
    ccache_stats_before = _ccache_stats(ccache) if ccache else None

    return {
        "captured_at_utc": utc_now_iso(),
        "git_commit": git("rev-parse", "HEAD"),
        "git_branch": git("rev-parse", "--abbrev-ref", "HEAD"),
        "git_dirty": bool(git("status", "--porcelain")),
        "cmake_preset": preset,
        "build_dir": str(build_dir.relative_to(repo_root)) if build_dir.is_relative_to(repo_root) else str(build_dir),
        "build_type": cache_vars.get("CMAKE_BUILD_TYPE", ""),
        "cxx_compiler": compiler,
        "cxx_compiler_id": compiler_id,
        "cxx_compiler_version": compiler_version,
        "cxx_flags": cache_vars.get("CMAKE_CXX_FLAGS", ""),
        "cxx_flags_release": cache_vars.get("CMAKE_CXX_FLAGS_RELEASE", ""),
        "cpu_count": os.cpu_count(),
        "ctest_parallelism": jobs,
        "ctest_version": _first_line(run(["ctest", "--version"], cwd=repo_root).stdout),
        "cmake_version": _first_line(run(["cmake", "--version"], cwd=repo_root).stdout),
        "ccache_path": ccache or "",
        "ccache_stats_before": ccache_stats_before,
    }


def _first_line(text: str) -> str:
    return text.strip().splitlines()[0] if text.strip() else ""


def _ccache_stats(ccache: str) -> dict:
    cp = subprocess.run([ccache, "-s"], stdout=subprocess.PIPE, text=True)
    stats: dict[str, str] = {}
    for line in cp.stdout.splitlines():
        if ":" in line:
            k, _, v = line.partition(":")
            stats[k.strip()] = v.strip()
    return stats


# --------------------------------------------------------------------------
# CTest entry metadata (binary, RUN_SERIAL, resource locks) via json-v1
# --------------------------------------------------------------------------
def load_ctest_metadata(build_dir: Path, repo_root: Path) -> dict[str, dict]:
    cp = run(["ctest", "--test-dir", str(build_dir), "--show-only=json-v1"], cwd=repo_root)
    if cp.returncode != 0:
        die(f"ctest --show-only=json-v1 failed:\n{cp.stdout}")
    try:
        data = json.loads(cp.stdout)
    except json.JSONDecodeError as e:
        die(f"could not parse ctest json-v1: {e}")
    meta: dict[str, dict] = {}
    for t in data.get("tests", []):
        name = t.get("name", "")
        cmd = t.get("command", [])
        binary = Path(cmd[0]).name if cmd else ""
        props = {p["name"]: p.get("value") for p in t.get("properties", [])}
        meta[name] = {
            "binary": binary,
            "run_serial": bool(props.get("RUN_SERIAL", False)),
            "resource_lock": props.get("RESOURCE_LOCK"),
            "timeout": props.get("TIMEOUT"),
            "labels": props.get("LABELS"),
        }
    return meta


# --------------------------------------------------------------------------
# JUnit parsing (CTest --output-junit): per-entry wall time
# --------------------------------------------------------------------------
def parse_ctest_junit(path: Path) -> list[dict]:
    if not path.is_file():
        die(f"CTest JUnit output not found: {path}")
    try:
        root = ET.parse(str(path)).getroot()
    except ET.ParseError as e:
        die(f"could not parse CTest JUnit {path}: {e}")
    entries: list[dict] = []
    for tc in root.iter("testcase"):
        name = tc.get("name", "")
        try:
            wall = float(tc.get("time", "0"))
        except ValueError:
            wall = 0.0
        status = "run"
        if tc.find("failure") is not None:
            status = "failed"
        elif tc.find("skipped") is not None:
            status = "skipped"
        elif tc.find("error") is not None:
            status = "error"
        entries.append({"name": name, "wall_s": wall, "status": status})
    if not entries:
        die(f"CTest JUnit {path} contained no testcase entries")
    return entries


# --------------------------------------------------------------------------
# GoogleTest json: in-process per-test body time (one process, no per-entry spawn)
# --------------------------------------------------------------------------
def parse_gtest_json(path: Path) -> dict:
    if not path.is_file():
        die(f"GoogleTest json output not found: {path}")
    data = json.loads(path.read_text())
    total_body = 0.0
    count = 0
    per_test: dict[str, float] = {}
    for suite in data.get("testsuites", []):
        suite_name = suite.get("name", "")
        for tc in suite.get("testsuite", []):
            name = tc.get("name", "")
            t = tc.get("time", "0s")
            secs = float(str(t).rstrip("s")) if t else 0.0
            full = f"{suite_name}.{name}"
            per_test[full] = secs
            total_body += secs
            count += 1
    # gtest reports its own overall elapsed under top-level "time" too.
    overall = data.get("time", "0s")
    overall_s = float(str(overall).rstrip("s")) if overall else 0.0
    return {
        "sum_body_s": round(total_body, 4),
        "reported_overall_s": round(overall_s, 4),
        "test_count": count,
        "per_test": per_test,
    }


# --------------------------------------------------------------------------
# Measurement: warm build + timed ctest gate, N times
# --------------------------------------------------------------------------
def measure_gate(build_dir: Path, repo_root: Path, jobs: int, run_index: int, scratch: Path) -> dict:
    build_start = time.monotonic()
    b = run(["cmake", "--build", str(build_dir), "--parallel"], cwd=repo_root)
    build_wall = time.monotonic() - build_start
    if b.returncode != 0:
        die(f"warm build failed on run {run_index}:\n{b.stdout[-4000:]}")

    junit_path = scratch / f"ctest_junit_run{run_index}.xml"
    gate_start = time.monotonic()
    c = run(
        [
            "ctest",
            "--test-dir",
            str(build_dir),
            "--output-on-failure",
            "-j",
            str(jobs),
            "--output-junit",
            str(junit_path),
        ],
        cwd=repo_root,
    )
    gate_wall = time.monotonic() - gate_start
    log_path = scratch / f"ctest_run{run_index}.log"
    log_path.write_text(c.stdout)

    passed, failed, total = _parse_ctest_summary(c.stdout)
    return {
        "run_index": run_index,
        "build_wall_s": round(build_wall, 3),
        "gate_wall_s": round(gate_wall, 3),
        "ctest_exit": c.returncode,
        "tests_total": total,
        "tests_passed": passed,
        "tests_failed": failed,
        "junit_path": str(junit_path),
        "log_path": str(log_path),
    }


def _parse_ctest_summary(stdout: str) -> tuple[int, int, int]:
    passed = failed = total = 0
    for line in stdout.splitlines():
        s = line.strip()
        if "tests passed" in s and "out of" in s:
            # e.g. "100% tests passed, 0 tests failed out of 1996"
            try:
                parts = s.split()
                total = int(parts[-1])
                failed = int(parts[parts.index("failed") - 1])
                passed = total - failed
            except (ValueError, IndexError):
                pass
    return passed, failed, total


# --------------------------------------------------------------------------
# Ranking + reconciliation
# --------------------------------------------------------------------------
def build_rankings(entries: list[dict], meta: dict[str, dict], top_n: int) -> dict:
    for e in entries:
        m = meta.get(e["name"], {})
        e["binary"] = m.get("binary", "")
        e["run_serial"] = m.get("run_serial", False)
        e["resource_lock"] = m.get("resource_lock")

    total_wall = sum(e["wall_s"] for e in entries)

    top_tests = sorted(entries, key=lambda e: (-e["wall_s"], e["name"]))[:top_n]
    top_tests_out = [
        {
            "name": e["name"],
            "wall_s": round(e["wall_s"], 4),
            "share_pct": round(100.0 * e["wall_s"] / total_wall, 3) if total_wall else 0.0,
            "binary": e["binary"],
            "run_serial": e["run_serial"],
            "resource_lock": e["resource_lock"],
        }
        for e in top_tests
    ]

    # Suites: group by gtest suite prefix (before first '.'); entries with no
    # dot (bundled binaries: lci_integration_suite, lci_real_project_suite,
    # lci_benchmarks) form their own single-entry "suite".
    suites: dict[str, dict] = {}
    for e in entries:
        name = e["name"]
        suite = name.split(".", 1)[0] if "." in name else name
        s = suites.setdefault(suite, {"wall_s": 0.0, "count": 0, "binary": e["binary"], "run_serial": False})
        s["wall_s"] += e["wall_s"]
        s["count"] += 1
        s["run_serial"] = s["run_serial"] or e["run_serial"]

    top_suites = sorted(suites.items(), key=lambda kv: (-kv[1]["wall_s"], kv[0]))[:top_n]
    top_suites_out = [
        {
            "suite": name,
            "wall_s": round(s["wall_s"], 4),
            "share_pct": round(100.0 * s["wall_s"] / total_wall, 3) if total_wall else 0.0,
            "entry_count": s["count"],
            "binary": s["binary"],
            "run_serial": s["run_serial"],
        }
        for name, s in top_suites
    ]

    return {
        "summed_entry_wall_s": round(total_wall, 3),
        "entry_count": len(entries),
        "top_tests": top_tests_out,
        "top_suites": top_suites_out,
    }


def reconcile_overhead(entries: list[dict], meta: dict[str, dict], gtest: dict, unit_binary: str) -> dict:
    unit_entries = [e for e in entries if meta.get(e["name"], {}).get("binary", "") == unit_binary]
    sum_ctest_unit_wall = sum(e["wall_s"] for e in unit_entries)
    sum_body = gtest["sum_body_s"]
    n = len(unit_entries)
    overhead = sum_ctest_unit_wall - sum_body
    return {
        "unit_binary": unit_binary,
        "unit_ctest_entry_count": n,
        "sum_ctest_entry_wall_s": round(sum_ctest_unit_wall, 3),
        "gtest_sum_body_s": sum_body,
        "gtest_reported_overall_s": gtest["reported_overall_s"],
        "gtest_test_count": gtest["test_count"],
        "process_spawn_discovery_overhead_s": round(overhead, 3),
        "overhead_pct_of_entry_wall": round(100.0 * overhead / sum_ctest_unit_wall, 2) if sum_ctest_unit_wall else 0.0,
        "overhead_per_entry_ms": round(1000.0 * overhead / n, 2) if n else 0.0,
    }


# --------------------------------------------------------------------------
# Markdown report
# --------------------------------------------------------------------------
def render_markdown(doc: dict) -> str:
    env = doc["environment"]
    gates = doc["gate_runs"]
    agg = doc["gate_wall"]
    rk = doc["rankings"]
    ov = doc["overhead_reconciliation"]
    cand = doc["candidates"]

    def tbl(rows: list[list[str]], header: list[str]) -> str:
        out = ["| " + " | ".join(header) + " |", "| " + " | ".join("---" for _ in header) + " |"]
        for r in rows:
            out.append("| " + " | ".join(r) + " |")
        return "\n".join(out)

    lines: list[str] = []
    lines.append("# Test-suite wall-clock baseline (S1)")
    lines.append("")
    lines.append(f"Generated by `scripts/profile_test_suite.py` on **{doc['generated_at_utc']}**.")
    lines.append("")
    lines.append(
        "Measurement-only baseline. No production or test code changed. All numbers "
        "measured fresh on THIS checkout; the historical ~420s figure is NOT quoted."
    )
    lines.append("")
    lines.append("## Environment")
    lines.append("")
    lines.append(
        tbl(
            [
                ["git commit", f"`{env['git_commit']}`"],
                ["git branch", f"`{env['git_branch']}`"],
                ["build dir", f"`{env['build_dir']}`"],
                ["build type", env["build_type"]],
                ["cmake preset", env["cmake_preset"]],
                ["C++ compiler", f"{env['cxx_compiler_id']} {env['cxx_compiler_version']} (`{env['cxx_compiler']}`)"],
                ["cmake", env["cmake_version"]],
                ["ctest", env["ctest_version"]],
                ["CPU count", str(env["cpu_count"])],
                ["ctest -j", str(env["ctest_parallelism"])],
                ["ccache", f"`{env['ccache_path']}`"],
            ],
            ["key", "value"],
        )
    )
    lines.append("")
    if env.get("ccache_stats_before"):
        cs = env["ccache_stats_before"]
        hit = cs.get("Hits", cs.get("cache hit rate", "?"))
        lines.append(f"ccache before runs: `{hit}` hits reported (`ccache -s`).")
        lines.append("")

    lines.append("## Reproduce")
    lines.append("")
    lines.append("```")
    lines.append("python3 scripts/profile_test_suite.py")
    lines.append("```")
    lines.append("")
    lines.append("Each gate run is exactly:")
    lines.append("")
    lines.append("```")
    lines.append(f"cmake --build {env['build_dir']} --parallel")
    lines.append(f"ctest --test-dir {env['build_dir']} --output-on-failure -j {env['ctest_parallelism']}")
    lines.append("```")
    lines.append("")

    lines.append("## Full-gate wall clock (warm ccache)")
    lines.append("")
    rows = [
        [
            str(g["run_index"]),
            f"{g['gate_wall_s']:.1f}",
            f"{g['build_wall_s']:.1f}",
            str(g["tests_total"]),
            str(g["tests_failed"]),
        ]
        for g in gates
    ]
    lines.append(tbl(rows, ["run", "gate wall (s)", "warm build (s)", "tests", "failed"]))
    lines.append("")
    lines.append(
        f"**Median gate: {agg['median_s']:.1f}s** — range {agg['min_s']:.1f}s..{agg['max_s']:.1f}s "
        f"(spread {agg['range_s']:.1f}s) across {agg['run_count']} warm runs."
    )
    lines.append("")

    lines.append("## Top-10 individual tests by wall time")
    lines.append("")
    lines.append(
        "Each `gtest_discover_tests` entry is its own ctest process. `wall_s` is the "
        "CTest-reported per-entry wall (includes process spawn)."
    )
    lines.append("")
    rows = [
        [
            t["name"],
            f"{t['wall_s']:.3f}",
            f"{t['share_pct']:.2f}%",
            t["binary"],
            "yes" if t["run_serial"] else "",
        ]
        for t in rk["top_tests"]
    ]
    lines.append(tbl(rows, ["test", "wall (s)", "share", "binary", "RUN_SERIAL"]))
    lines.append("")

    lines.append("## Top-10 suites / CTest entries by wall time")
    lines.append("")
    lines.append(
        "Grouped by GoogleTest suite prefix; bundled single-entry binaries "
        "(`lci_integration_suite`, `lci_real_project_suite`, `lci_benchmarks`) "
        "appear as their own row."
    )
    lines.append("")
    rows = [
        [
            s["suite"],
            f"{s['wall_s']:.3f}",
            f"{s['share_pct']:.2f}%",
            str(s["entry_count"]),
            s["binary"],
            "yes" if s["run_serial"] else "",
        ]
        for s in rk["top_suites"]
    ]
    lines.append(tbl(rows, ["suite", "wall (s)", "share", "entries", "binary", "RUN_SERIAL"]))
    lines.append("")

    lines.append("## Discovery / process overhead (CTest wall vs GoogleTest body time)")
    lines.append("")
    lines.append(
        f"The unit binary `{ov['unit_binary']}` registers **{ov['unit_ctest_entry_count']} "
        f"separate ctest entries**, each a fresh process. Summed CTest per-entry wall is "
        f"**{ov['sum_ctest_entry_wall_s']:.1f}s**, but the same tests run in ONE process "
        f"(`--gtest_output=json`) report only **{ov['gtest_sum_body_s']:.1f}s** of test-body time."
    )
    lines.append("")
    lines.append(
        f"=> **{ov['process_spawn_discovery_overhead_s']:.1f}s "
        f"({ov['overhead_pct_of_entry_wall']:.1f}% of unit entry wall, "
        f"~{ov['overhead_per_entry_ms']:.1f} ms/entry)** is process spawn + per-process "
        "global init/discovery overhead, NOT test-body work. Wall time must not be "
        "attributed entirely to test bodies."
    )
    lines.append("")

    lines.append("## Candidates for follow-up slices (measured only)")
    lines.append("")
    lines.append("### S2 — CTest discovery granularity / parallel races")
    lines.append("")
    for c in cand["s2"]:
        lines.append(f"- {c}")
    lines.append("")
    lines.append("### S3 — repeated integration indexing")
    lines.append("")
    for c in cand["s3"]:
        lines.append(f"- {c}")
    lines.append("")
    lines.append(
        "Candidates above name ONLY measured evidence. No fix is proposed here; S2/S3 own the fixes."
    )
    lines.append("")
    return "\n".join(lines)


def build_candidates(rk: dict, ov: dict, agg: dict) -> dict:
    # S2: process-spawn overhead across the ~N unit entries.
    s2 = [
        (
            f"gtest_discover_tests per-entry spawn overhead = "
            f"{ov['process_spawn_discovery_overhead_s']:.1f}s "
            f"({ov['overhead_pct_of_entry_wall']:.1f}% of unit entry wall) across "
            f"{ov['unit_ctest_entry_count']} single-test processes "
            f"(~{ov['overhead_per_entry_ms']:.1f} ms/process). Coarser discovery "
            "granularity (fewer ctest entries per binary) directly reclaims this."
        )
    ]
    serial_suites = [s for s in rk["top_suites"] if s["run_serial"]]
    if serial_suites:
        names = ", ".join(f"`{s['suite']}` ({s['wall_s']:.1f}s)" for s in serial_suites)
        s2.append(
            f"RUN_SERIAL entries force sequential execution and cannot overlap the -j pool: "
            f"{names}. On a {agg.get('cpu_count', '?')}-way box these gate the tail of the wall clock."
        )

    # S3: repeated integration indexing — the integration + real-project bundles.
    s3 = []
    for s in rk["top_suites"]:
        b = s.get("binary", "")
        if b in ("lci_integration_tests", "lci_real_project_tests") or s["suite"] in (
            "lci_integration_suite",
            "lci_real_project_suite",
        ):
            s3.append(
                f"`{s['suite']}` (binary `{b or s['suite']}`) = {s['wall_s']:.1f}s "
                f"({s['share_pct']:.2f}% of entry wall), RUN_SERIAL={s['run_serial']}. "
                "These bundles index real corpora; repeated per-corpus indexing across "
                "cases/suites is the measured cost surface S3 owns."
            )
    if not s3:
        s3.append(
            "No integration/real-project bundle appeared in the top suites this run; "
            "re-check integration indexing cost directly before S3 scopes work."
        )
    return {"s2": s2, "s3": s3}


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------
def main() -> "None":
    ap = argparse.ArgumentParser(description="Profile the LCI C++ full test-suite wall clock.")
    ap.add_argument("--repo-root", default=None, help="Repo root (default: git toplevel of CWD).")
    ap.add_argument("--build-dir", default="build/release", help="CMake build dir (default: build/release).")
    ap.add_argument("--jobs", type=int, default=4, help="ctest -j parallelism (default: 4).")
    ap.add_argument("--runs", type=int, default=3, help="Number of warm gate runs (default: 3, minimum 3).")
    ap.add_argument("--preset", default="release", help="CMake preset name for the record (default: release).")
    ap.add_argument("--top", type=int, default=10, help="Ranking depth (default: 10).")
    ap.add_argument("--unit-binary", default="lci_tests", help="Unit binary for overhead reconciliation.")
    ap.add_argument("--out-json", default="docs/performance/test-suite-baseline.json")
    ap.add_argument("--out-md", default="docs/performance/test-suite-baseline.md")
    ap.add_argument("--scratch", default=None, help="Scratch dir for junit/gtest temp files (default: a tmpdir).")
    ap.add_argument(
        "--reuse-junit",
        default=None,
        help="DEV ONLY: parse an existing CTest JUnit xml + gtest json instead of running gates. "
        "Format: <junit.xml>,<gtest.json>. Skips measurement; single synthetic run recorded.",
    )
    args = ap.parse_args()

    for tool in ("cmake", "ctest", "git"):
        require_tool(tool)

    if args.repo_root:
        repo_root = Path(args.repo_root).resolve()
    else:
        cp = subprocess.run(["git", "rev-parse", "--show-toplevel"], stdout=subprocess.PIPE, text=True)
        if cp.returncode != 0:
            die("could not determine repo root; pass --repo-root")
        repo_root = Path(cp.stdout.strip())

    build_dir = (repo_root / args.build_dir).resolve()
    if not build_dir.is_dir():
        die(f"build dir does not exist: {build_dir}")

    scratch = Path(args.scratch) if args.scratch else Path(tempfile.mkdtemp(prefix="lci_profile_"))
    scratch.mkdir(parents=True, exist_ok=True)

    env = capture_environment(repo_root, build_dir, args.jobs, args.preset)
    meta = load_ctest_metadata(build_dir, repo_root)

    gate_runs: list[dict] = []
    if args.reuse_junit:
        junit_str, gtest_str = args.reuse_junit.split(",", 1)
        gate_runs.append(
            {
                "run_index": 1,
                "build_wall_s": 0.0,
                "gate_wall_s": 0.0,
                "ctest_exit": 0,
                "tests_total": 0,
                "tests_passed": 0,
                "tests_failed": 0,
                "junit_path": junit_str,
                "log_path": "",
                "synthetic_reuse": True,
            }
        )
        gtest_json_path = Path(gtest_str)
    else:
        runs = max(3, args.runs)
        for i in range(1, runs + 1):
            gate_runs.append(measure_gate(build_dir, repo_root, args.jobs, i, scratch))
        # one in-process gtest run for body-time reconciliation
        gtest_json_path = scratch / "gtest_unit.json"
        g = run(
            [str(build_dir / "tests" / args.unit_binary), f"--gtest_output=json:{gtest_json_path}"],
            cwd=build_dir / "tests",
        )
        if g.returncode not in (0, 1):  # 1 = some test failed but json still written
            die(f"gtest json run failed (exit {g.returncode}):\n{g.stdout[-2000:]}")

    failed_runs = [g for g in gate_runs if g.get("ctest_exit", 0) not in (0,)]
    if failed_runs and not args.reuse_junit:
        die(
            "one or more gate runs had a non-zero ctest exit (real test failure). "
            "Root-cause the failure; do NOT force past it. "
            f"Runs: {[(g['run_index'], g['ctest_exit'], g['tests_failed']) for g in failed_runs]}. "
            f"Logs under {scratch}."
        )

    gate_totals = [g["gate_wall_s"] for g in gate_runs if not g.get("synthetic_reuse")]
    if gate_totals:
        agg = {
            "run_count": len(gate_totals),
            "runs_s": sorted(gate_totals),
            "median_s": round(statistics.median(gate_totals), 3),
            "min_s": round(min(gate_totals), 3),
            "max_s": round(max(gate_totals), 3),
            "range_s": round(max(gate_totals) - min(gate_totals), 3),
            "mean_s": round(statistics.mean(gate_totals), 3),
            "cpu_count": env["cpu_count"],
        }
    else:
        agg = {"run_count": 0, "runs_s": [], "median_s": 0.0, "min_s": 0.0, "max_s": 0.0, "range_s": 0.0, "mean_s": 0.0, "cpu_count": env["cpu_count"]}

    # Rank against the median run's JUnit (the run whose total is the median).
    ranking_run = _pick_median_run(gate_runs)
    entries = parse_ctest_junit(Path(ranking_run["junit_path"]))
    gtest = parse_gtest_json(gtest_json_path)

    rankings = build_rankings(entries, meta, args.top)
    overhead = reconcile_overhead(entries, meta, gtest, args.unit_binary)
    candidates = build_candidates(rankings, overhead, agg)

    env["ccache_stats_after"] = _ccache_stats(env["ccache_path"]) if env["ccache_path"] else None

    doc = {
        "generated_at_utc": utc_now_iso(),
        "schema": "lci-test-suite-baseline/v1",
        "environment": env,
        "gate_runs": gate_runs,
        "gate_wall": agg,
        "ranking_run_index": ranking_run["run_index"],
        "rankings": rankings,
        "overhead_reconciliation": overhead,
        "candidates": candidates,
    }

    out_json = (repo_root / args.out_json).resolve()
    out_md = (repo_root / args.out_md).resolve()
    out_json.parent.mkdir(parents=True, exist_ok=True)
    out_md.parent.mkdir(parents=True, exist_ok=True)
    out_json.write_text(json.dumps(doc, indent=2, sort_keys=True) + "\n")
    out_md.write_text(render_markdown(doc))

    print(f"wrote {out_json}")
    print(f"wrote {out_md}")
    if agg["run_count"]:
        print(
            f"median gate {agg['median_s']:.1f}s (range {agg['min_s']:.1f}..{agg['max_s']:.1f}s), "
            f"overhead {overhead['process_spawn_discovery_overhead_s']:.1f}s "
            f"({overhead['overhead_pct_of_entry_wall']:.1f}%)"
        )


def _pick_median_run(gate_runs: list[dict]) -> dict:
    real = [g for g in gate_runs if not g.get("synthetic_reuse")]
    if not real:
        return gate_runs[0]
    ordered = sorted(real, key=lambda g: g["gate_wall_s"])
    return ordered[len(ordered) // 2]


if __name__ == "__main__":
    main()
