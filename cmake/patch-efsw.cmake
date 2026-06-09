# Idempotent, cross-platform patch for vendored efsw (FetchContent).
#
# TSan-confirmed data race: FileWatcherInotify's destructor spins
# `while (mIsTakingAction)` to hand off with its inotify worker thread (which
# writes the flag in handleAction), but mIsTakingAction is a plain bool —
# unsynchronised cross-thread access. efsw already wraps mInitOK in its own
# Atomic<bool>; make mIsTakingAction match.
#
# Invoked as the FetchContent PATCH_COMMAND via `cmake -P`, with the working
# directory set to the efsw source tree. Uses file(READ/WRITE) instead of
# `sed -i` so it works identically on GNU, BSD/macOS, and Windows (the old
# bash+sed script broke on macOS's BSD sed and on Windows runners without
# bash). After the substitution the original pattern is gone, so re-running is
# a no-op.
set(_file "src/efsw/FileWatcherInotify.hpp")
if(NOT EXISTS "${_file}")
    message(FATAL_ERROR "patch-efsw: ${_file} not found (cwd: ${CMAKE_CURRENT_LIST_DIR})")
endif()

file(READ "${_file}" _contents)
string(REPLACE "bool mIsTakingAction;" "Atomic<bool> mIsTakingAction;" _patched "${_contents}")
if(NOT _patched STREQUAL _contents)
    file(WRITE "${_file}" "${_patched}")
    message(STATUS "patch-efsw: made mIsTakingAction atomic")
else()
    message(STATUS "patch-efsw: already patched (no-op)")
endif()
