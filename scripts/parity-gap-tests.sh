#!/usr/bin/env bash
set -euo pipefail

# Fast-iteration runner for the cross-platform parity gap.
#
# These are the suites that are (or were) excluded on the Windows / macOS CI
# legs because they exercise platform-divergent code or hit a platform-specific
# defect. On Linux they all pass, so this script lets you iterate on a parity
# fix in <1 min instead of waiting for a Windows/macOS CI round-trip.
#
# The filter strings here are the SINGLE SOURCE OF TRUTH mirrored by
# .github/workflows/ci.yml (Windows + macOS "unit suite" steps) and
# docs/superpowers/plans/2026-06-09-windows-macos-release-followups.md.
# When a gap is closed, drop the suite from BOTH the CI filter and here.
#
# Usage:
#   scripts/parity-gap-tests.sh            # shims + all current parity-gap suites
#   scripts/parity-gap-tests.sh --win      # only the Windows-excluded suites
#   scripts/parity-gap-tests.sh --mac      # only the macOS-excluded suites
#   scripts/parity-gap-tests.sh --no-build # skip the incremental build
#
# Env:
#   BUILD_DIR   build directory (default: build)

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

BUILD_DIR="${BUILD_DIR:-build}"
DO_BUILD=1
SCOPE="all"
for arg in "$@"; do
  case "$arg" in
    --win) SCOPE="win" ;;
    --mac) SCOPE="mac" ;;
    --no-build) DO_BUILD=0 ;;
    -h|--help) sed -n '3,22p' "$0"; exit 0 ;;
    *) echo "unknown arg: $arg" >&2; exit 2 ;;
  esac
done

# Platform-divergent shims — always fast to verify, both sides implemented.
SHIMS='PortableTest.*:SubprocessTest.*'

# Currently excluded on the Windows CI leg (TCP-vs-AF_UNIX socket fixtures +
# POSIX path normalization in report_to_json).
WIN_GAP='ServerTest.*:ClientTest.*:GitReportToJson.*:CodeInsightGitTest.*:ContextHandlerFixture.*:HandlersFixture.*:ExploreIndexTestFixture.*'

# Currently excluded on the macOS CI leg. FileWatcherTest = efsw FSEvents
# teardown abort (followups #3, still open). The Go suites were re-enabled
# after the unsequenced move+is_exported fix (followups #5) and are kept here
# so a regression is caught locally before it reaches the macOS leg.
MAC_GAP='FileWatcherTest.*:GoExtractorTest.*:GoLinkerIntegrationTest.*:AllLinkerIntegrationTest.GoMultiFileProject'

case "$SCOPE" in
  win) FILTER="$SHIMS:$WIN_GAP" ;;
  mac) FILTER="$SHIMS:$MAC_GAP" ;;
  all) FILTER="$SHIMS:$WIN_GAP:$MAC_GAP" ;;
esac

BIN="$BUILD_DIR/tests/lci_tests"

if [[ "$DO_BUILD" == 1 ]]; then
  echo -e "${YELLOW}building lci_tests...${NC}"
  cmake --build "$BUILD_DIR" --target lci_tests -j"$(nproc 2>/dev/null || echo 4)"
fi

[[ -x "$BIN" ]] || { echo -e "${RED}missing $BIN — build first (omit --no-build)${NC}" >&2; exit 1; }

echo -e "${YELLOW}scope=$SCOPE${NC}"
if "$BIN" "--gtest_filter=$FILTER"; then
  echo -e "${GREEN}parity-gap suites green${NC}"
else
  echo -e "${RED}parity-gap suites FAILED${NC}" >&2
  exit 1
fi
