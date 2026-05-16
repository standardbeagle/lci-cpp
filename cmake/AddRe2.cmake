# Adds re2 via FetchContent without polluting the parent's install manifest.
#
# RE2 unconditionally emits install(EXPORT re2Targets), which fails for us
# because the FetchContent'd abseil targets aren't in any export set. We only
# consume re2::re2 in-tree (never `cmake --install`).
#
# CMake's function() override mechanism saves the previous definition with a
# leading underscore. We shadow install() with a no-op while re2's CMakeLists
# runs, then forward install() back to the underscored original. This keeps
# the rest of the project's install rules (if any) intact.
include(FetchContent)

set(RE2_BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(re2
    GIT_REPOSITORY https://github.com/google/re2.git
    GIT_TAG 2024-07-02
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(re2)
if(NOT re2_POPULATED)
    FetchContent_Populate(re2)
    # Shadow: install() becomes a no-op; the original is reachable as _install
    # (CMake stashes it automatically when we redefine).
    function(install)
        # intentionally empty — discard RE2's install rules
    endfunction()
    add_subdirectory(${re2_SOURCE_DIR} ${re2_BINARY_DIR} EXCLUDE_FROM_ALL)
    # Restore install() by forwarding to the stashed original.
    function(install)
        _install(${ARGV})
    endfunction()
endif()
