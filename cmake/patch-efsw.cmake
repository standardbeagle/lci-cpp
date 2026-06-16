# Idempotent, cross-platform patches for vendored efsw (FetchContent).
#
# Invoked as the FetchContent PATCH_COMMAND via `cmake -P`, with the working
# directory set to the efsw source tree. Uses file(READ/WRITE) instead of
# `sed -i` so it works identically on GNU, BSD/macOS, and Windows (the old
# bash+sed script broke on macOS's BSD sed and on Windows runners without
# bash). Each patch is guarded so re-running is a no-op.
#
# ----------------------------------------------------------------------------
# Patch 1 (Linux/inotify) — TSan-confirmed data race.
# FileWatcherInotify's destructor spins `while (mIsTakingAction)` to hand off
# with its inotify worker thread (which writes the flag in handleAction), but
# mIsTakingAction is a plain bool — unsynchronised cross-thread access. efsw
# already wraps mInitOK in its own Atomic<bool>; make mIsTakingAction match.
# ----------------------------------------------------------------------------
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
    message(STATUS "patch-efsw: inotify already patched (no-op)")
endif()

# ----------------------------------------------------------------------------
# Patch 2 (macOS/FSEvents) — use-after-free abort on watcher teardown
# ("libc++abi: mutex lock failed", exit 134). The macOS analogue of patch 1.
#
# WatcherFSEvents::init() creates a dispatch queue as a LOCAL and binds the
# FSEventStream to it (FSEventStreamSetDispatchQueue), but never stores it. The
# destructor calls FSEventStreamStop/Invalidate/Release, none of which wait for
# a callback already executing on that queue. That in-flight FSEventCallback
# then dereferences the just-deleted WatcherFSEvents and calls into the
# listener, locking a mutex whose owner is being destroyed -> abort.
#
# Fix: keep the dispatch queue as a member, and in the destructor drain it
# synchronously (dispatch_sync_f on the serial queue blocks until the in-flight
# callback returns) before releasing it. dispatch_release also plugs the queue
# leak. Guarded by the presence of the "FSQueue" member name.
# ----------------------------------------------------------------------------
set(_hpp "src/efsw/WatcherFSEvents.hpp")
set(_cpp "src/efsw/WatcherFSEvents.cpp")
if(EXISTS "${_hpp}" AND EXISTS "${_cpp}")
    file(READ "${_hpp}" _h)
    if(NOT _h MATCHES "FSQueue")
        string(REPLACE
            "#include <efsw/WatcherGeneric.hpp>"
            "#include <efsw/WatcherGeneric.hpp>\n#include <dispatch/dispatch.h>"
            _h "${_h}")
        string(REPLACE
            "\tFSEventStreamRef FSStream;"
            "\tFSEventStreamRef FSStream;\n\tdispatch_queue_t FSQueue;"
            _h "${_h}")
        file(WRITE "${_hpp}" "${_h}")
        message(STATUS "patch-efsw: added FSQueue member to WatcherFSEvents.hpp")
    else()
        message(STATUS "patch-efsw: FSEvents header already patched (no-op)")
    endif()

    file(READ "${_cpp}" _c)
    if(NOT _c MATCHES "FSQueue")
        # Initialise the new member in both constructors.
        string(REPLACE
            "FSStream( NULL ), WatcherGen( NULL ) {}"
            "FSStream( NULL ), FSQueue( NULL ), WatcherGen( NULL ) {}"
            _c "${_c}")
        string(REPLACE
            "\tFSStream( NULL ),\n\tWatcherGen( NULL ) {}"
            "\tFSStream( NULL ),\n\tFSQueue( NULL ),\n\tWatcherGen( NULL ) {}"
            _c "${_c}")
        # Store the dispatch queue instead of leaking a local.
        string(REPLACE
            "\tdispatch_queue_t queue = dispatch_queue_create(NULL, NULL);"
            "\tFSQueue = dispatch_queue_create(NULL, NULL);"
            _c "${_c}")
        string(REPLACE
            "\tFSEventStreamSetDispatchQueue(FSStream, queue);"
            "\tFSEventStreamSetDispatchQueue(FSStream, FSQueue);"
            _c "${_c}")
        # Define the drain helper just before the destructor (stays in namespace efsw).
        string(REPLACE
            "WatcherFSEvents::~WatcherFSEvents() {"
            "static void efsw_fsevents_drain_noop( void* ) {}\n\nWatcherFSEvents::~WatcherFSEvents() {"
            _c "${_c}")
        # Drain + release the queue in the destructor before deleting state.
        string(REPLACE
            "\t\tFSEventStreamRelease( FSStream );\n\t}\n\n\tefSAFE_DELETE( WatcherGen );"
            "\t\tFSEventStreamRelease( FSStream );\n\t}\n\n\tif ( NULL != FSQueue ) {\n\t\tdispatch_sync_f( FSQueue, NULL, &efsw_fsevents_drain_noop );\n\t\tdispatch_release( FSQueue );\n\t\tFSQueue = NULL;\n\t}\n\n\tefSAFE_DELETE( WatcherGen );"
            _c "${_c}")
        file(WRITE "${_cpp}" "${_c}")
        message(STATUS "patch-efsw: drained+released FSEvents dispatch queue on teardown")
    else()
        message(STATUS "patch-efsw: FSEvents source already patched (no-op)")
    endif()
else()
    message(STATUS "patch-efsw: FSEvents backend not present (no-op)")
endif()
