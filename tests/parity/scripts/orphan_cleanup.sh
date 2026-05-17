#!/usr/bin/env bash
# Setup/teardown helper for parity tests.
#
# Modes:
#   setup    — kill any lingering lci-cpp / lci-go / parity_runner
#              children left over from a prior crashed run, plus stale
#              /tmp/lci-*.sock files.
#   teardown — pkill any procs that survived our run, scrub sockets.
#   verify   — fail (exit 1) if any orphan procs or sockets remain.
#
# Used by tests/parity/CMakeLists.txt via CTest fixtures.

set -u

mode="${1:-setup}"
exit_code=0

# Match processes that look like lci server children spawned by parity tests.
match_proc() {
    pgrep -f 'lci --root .*parity/corpora' 2>/dev/null
    pgrep -f 'lci-linux.*--root .*parity/corpora' 2>/dev/null
}

count_orphans() {
    match_proc | wc -l
}

# Count only sockets currently bound by an lci server whose --root is
# inside the parity corpora tree. Without this scoping, unrelated lci
# daemons on the same machine (e.g. an editor-integration server rooted
# at the user's main project) would have their socket files counted as
# parity orphans because they share the global /tmp namespace.
#
# Implementation: lsof reports each socket file with its owning pid;
# cross-check those pids against the same `match_proc` filter used for
# the process count. Falls back to the old broad count if lsof is
# missing — bare WSL/CI containers always include it.
count_sockets() {
    local socks
    socks="$(ls /tmp/lci-*-*.sock /tmp/lci-server-*.sock 2>/dev/null)"
    if [ -z "$socks" ]; then
        echo 0
        return
    fi
    if ! command -v lsof >/dev/null 2>&1; then
        # No lsof — fall back to raw count so we still flag *something*.
        echo "$socks" | wc -l
        return
    fi
    local parity_pids
    parity_pids="$(match_proc | sort -u | tr '\n' '|' | sed 's/|$//')"
    if [ -z "$parity_pids" ]; then
        # No parity-rooted procs alive → no parity sockets either.
        echo 0
        return
    fi
    local n=0
    local sock
    for sock in $socks; do
        local owners
        owners="$(lsof -t "$sock" 2>/dev/null | sort -u)"
        local p
        for p in $owners; do
            if echo "$p" | grep -qE "^(${parity_pids})$"; then
                n=$((n + 1))
                break
            fi
        done
    done
    echo "$n"
}

case "$mode" in
    setup|teardown)
        pids="$(match_proc | sort -u)"
        if [ -n "$pids" ]; then
            echo "$0: killing orphan lci pids: $pids" >&2
            echo "$pids" | xargs -r kill -TERM 2>/dev/null
            sleep 1
            echo "$pids" | xargs -r kill -KILL 2>/dev/null || true
        fi
        rm -f /tmp/lci-*-*.sock /tmp/lci-server-*.sock 2>/dev/null
        ;;
    verify)
        n_proc="$(count_orphans)"
        n_sock="$(count_sockets)"
        if [ "$n_proc" -gt 0 ] || [ "$n_sock" -gt 0 ]; then
            echo "$0: orphan check FAILED: $n_proc procs, $n_sock sockets" >&2
            match_proc | xargs -r ps -p >&2 || true
            # Raw socket inventory for diagnostic context — count_sockets
            # has already scoped the *count* to parity-owned ones.
            ls /tmp/lci-*-*.sock /tmp/lci-server-*.sock 2>/dev/null >&2 || true
            exit_code=1
        else
            echo "$0: clean ($n_proc procs, $n_sock sockets)"
        fi
        ;;
    *)
        echo "usage: $0 {setup|teardown|verify}" >&2
        exit 2
        ;;
esac

exit "$exit_code"
