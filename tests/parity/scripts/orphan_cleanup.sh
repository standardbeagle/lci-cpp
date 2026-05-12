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

count_sockets() {
    ls /tmp/lci-*-*.sock /tmp/lci-server-*.sock 2>/dev/null | wc -l
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
