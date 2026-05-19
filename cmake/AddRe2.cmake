# Adds re2 via FetchContent without polluting the parent's install manifest.
#
# RE2 unconditionally emits install(EXPORT re2Targets), which fails for us
# because the FetchContent'd abseil targets aren't in any export set. We only
# consume re2::re2 in-tree (never `cmake --install`).
#
# We shadow install() with a guarded wrapper. While LCI_SUPPRESS_INSTALL is
# truthy, the wrapper does nothing; otherwise it forwards to the original
# builtin (reachable as _install after the override). Critically: we override
# install() exactly ONCE — re-defining a second time replaces _install with
# the previous (noop) override, which would silently drop the root project's
# real install(TARGETS lci ...) rule. The guard variable lets us toggle
# behavior without re-defining.
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
    set(LCI_SUPPRESS_INSTALL ON)
    function(install)
        if(NOT LCI_SUPPRESS_INSTALL)
            _install(${ARGV})
        endif()
    endfunction()
    add_subdirectory(${re2_SOURCE_DIR} ${re2_BINARY_DIR} EXCLUDE_FROM_ALL)
    set(LCI_SUPPRESS_INSTALL OFF)
endif()
