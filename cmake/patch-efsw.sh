#!/usr/bin/env bash
# Patch vendored efsw (FetchContent) for a TSan-confirmed data race:
# FileWatcherInotify's destructor spins `while (mIsTakingAction)` to hand off
# with its inotify worker thread (which writes the flag in handleAction), but
# mIsTakingAction is a plain bool — unsynchronised cross-thread access. efsw
# already wraps mInitOK in its own Atomic<bool>; make mIsTakingAction match.
#
# Runs in the efsw SOURCE_DIR (ExternalProject PATCH_COMMAND cwd). Idempotent:
# after the substitution the original pattern is gone, so re-running is a no-op.
set -euo pipefail
sed -i 's/bool mIsTakingAction;/Atomic<bool> mIsTakingAction;/' src/efsw/FileWatcherInotify.hpp
