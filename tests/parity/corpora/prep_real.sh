#!/usr/bin/env bash
# Symlinks the three real-repo corpora into tests/parity/corpora/.
# Idempotent: re-running is a no-op if the symlinks already point correctly.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

link_if_needed() {
    local target="$1" link="$2"
    if [[ ! -e "$target" ]]; then
        echo "WARN: $target missing — skipping $link"
        return 0
    fi
    if [[ -L "$link" && "$(readlink "$link")" == "$target" ]]; then
        return 0
    fi
    rm -f "$link"
    ln -s "$target" "$link"
    echo "linked: $link -> $target"
}

LCI_GO_REPO="${LCI_GO_REPO:-/home/beagle/work/core/lci}"
LCI_CPP_REPO="${LCI_CPP_REPO:-/home/beagle/work/core/lci-cpp}"
LCI_TEST_REPO="${LCI_TEST_REPO:-/home/beagle/work/core/lci-test}"

link_if_needed "$LCI_GO_REPO"   "$HERE/lci-go-repo"
link_if_needed "$LCI_CPP_REPO"  "$HERE/lci-cpp-repo"
link_if_needed "$LCI_TEST_REPO" "$HERE/lci-test"

echo "OK"
