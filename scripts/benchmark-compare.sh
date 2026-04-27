#!/usr/bin/env bash
set -euo pipefail

# Benchmark comparison: C++ LCI vs Go LCI
# Measures indexing throughput, search latency, memory usage, and startup time.
#
# Usage:
#   ./scripts/benchmark-compare.sh [project-dir]
#
# Requirements:
#   - Go lci binary in PATH (or LCI_GO_BIN env var)
#   - C++ lci binary built at build/release/src/lci (or LCI_CPP_BIN env var)
#   - A project directory to index (defaults to current directory)

PROJECT_DIR="${1:-.}"
PROJECT_DIR="$(cd "$PROJECT_DIR" && pwd)"

GO_BIN="${LCI_GO_BIN:-$(command -v lci 2>/dev/null || echo "")}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CPP_BIN="${LCI_CPP_BIN:-${SCRIPT_DIR}/../build/release/src/lci}"

if [ -z "$GO_BIN" ]; then
    echo "Error: Go lci binary not found. Set LCI_GO_BIN or add to PATH." >&2
    exit 1
fi

if [ ! -x "$CPP_BIN" ]; then
    echo "Error: C++ lci binary not found at $CPP_BIN" >&2
    echo "Build with: cmake --build build/release --parallel" >&2
    exit 1
fi

ITERATIONS="${BENCH_ITERATIONS:-5}"
RESULTS_DIR=$(mktemp -d)
trap 'rm -rf "$RESULTS_DIR"' EXIT

echo "=== LCI Performance Comparison ==="
echo "Project:    $PROJECT_DIR"
echo "Go binary:  $GO_BIN"
echo "C++ binary: $CPP_BIN"
echo "Iterations: $ITERATIONS"
echo ""

measure_time() {
    local label="$1"
    shift
    local total=0
    local times=()
    for i in $(seq 1 "$ITERATIONS"); do
        local start end elapsed
        start=$(date +%s%N)
        "$@" > /dev/null 2>&1
        end=$(date +%s%N)
        elapsed=$(( (end - start) / 1000000 ))
        times+=("$elapsed")
        total=$((total + elapsed))
    done

    IFS=$'\n' sorted=($(sort -n <<<"${times[*]}")); unset IFS
    local p50_idx=$(( ITERATIONS / 2 ))
    local p95_idx=$(( ITERATIONS * 95 / 100 ))
    local p99_idx=$(( ITERATIONS - 1 ))
    [ "$p95_idx" -ge "$ITERATIONS" ] && p95_idx=$((ITERATIONS - 1))

    local avg=$((total / ITERATIONS))
    printf "  %-12s avg=%4dms  p50=%4dms  p95=%4dms  p99=%4dms\n" \
        "$label" "$avg" "${sorted[$p50_idx]}" "${sorted[$p95_idx]}" "${sorted[$p99_idx]}"
}

measure_rss() {
    local label="$1"
    shift
    local rss
    if command -v /usr/bin/time > /dev/null 2>&1; then
        rss=$(/usr/bin/time -v "$@" 2>&1 | grep "Maximum resident" | awk '{print $NF}')
        printf "  %-12s RSS=%s KB\n" "$label" "$rss"
    else
        printf "  %-12s RSS=n/a (install GNU time)\n" "$label"
    fi
}

echo "--- Startup Time (--version) ---"
measure_time "Go" "$GO_BIN" --version
measure_time "C++" "$CPP_BIN" --version
echo ""

echo "--- Indexing (list files) ---"
measure_time "Go" "$GO_BIN" list -r "$PROJECT_DIR"
measure_time "C++" "$CPP_BIN" list -r "$PROJECT_DIR"
echo ""

echo "--- Search Latency (pattern: 'func') ---"
measure_time "Go" "$GO_BIN" search func -r "$PROJECT_DIR"
measure_time "C++" "$CPP_BIN" search func -r "$PROJECT_DIR"
echo ""

echo "--- Search Latency (pattern: 'handleRequest') ---"
measure_time "Go" "$GO_BIN" search handleRequest -r "$PROJECT_DIR"
measure_time "C++" "$CPP_BIN" search handleRequest -r "$PROJECT_DIR"
echo ""

echo "--- Grep Throughput ---"
measure_time "Go" "$GO_BIN" grep "import" -r "$PROJECT_DIR"
measure_time "C++" "$CPP_BIN" grep "import" -r "$PROJECT_DIR"
echo ""

echo "--- Memory Usage (search) ---"
measure_rss "Go" "$GO_BIN" search func -r "$PROJECT_DIR"
measure_rss "C++" "$CPP_BIN" search func -r "$PROJECT_DIR"
echo ""

echo "--- Google Benchmark Suite (C++ only) ---"
BENCH_BIN="${SCRIPT_DIR}/../build/release/tests/lci_benchmarks"
if [ -x "$BENCH_BIN" ]; then
    "$BENCH_BIN" --benchmark_format=console --benchmark_min_time=0.1s
else
    echo "  (not built - run: cmake --build build/release --target lci_benchmarks)"
fi
echo ""

echo "=== Comparison Complete ==="
