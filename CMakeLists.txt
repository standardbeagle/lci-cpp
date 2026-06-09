cmake_minimum_required(VERSION 3.25)

project(lci
    VERSION 0.6.0
    DESCRIPTION "Lightning Code Index - fast semantic code search"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# macOS universal binary support (arm64 + x86_64)
option(LCI_UNIVERSAL_BINARY "Build macOS universal binary (arm64 + x86_64)" OFF)
if(LCI_UNIVERSAL_BINARY AND APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures" FORCE)
endif()

# Compiler detection and warning flag list.
#
# Flags live in LCI_WARNING_FLAGS and are applied PER-TARGET in src/ and tests/,
# NOT at directory scope. Directory-scope add_compile_options propagates into
# FetchContent vendored libraries (tree-sitter, re2, abseil, nlohmann_json_schema_validator,
# etc.) which have their own warning hygiene policies — combining -Werror with
# their existing warnings tanks the build for reasons orthogonal to our code.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "13.0")
        message(FATAL_ERROR "GCC 13+ required, found ${CMAKE_CXX_COMPILER_VERSION}")
    endif()
    # -Wno-unused-{parameter,function} on purpose: stubs / deferred-handler
    # surfaces are tracked in Dart with explicit follow-up tasks (Karpathy rule
    # 6 — surface signal, don't silence). Compile-time noise on those interferes
    # with -Werror gating on the warnings that DO catch real bugs.
    set(LCI_WARNING_FLAGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion
                          -Wno-unused-parameter -Wno-unused-function)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    # Apple Clang uses its own version numbering (e.g. 15.x ships with C++20 support)
    # -Wno-unused-{parameter,function} on purpose: stubs / deferred-handler
    # surfaces are tracked in Dart with explicit follow-up tasks (Karpathy rule
    # 6 — surface signal, don't silence). Compile-time noise on those interferes
    # with -Werror gating on the warnings that DO catch real bugs.
    set(LCI_WARNING_FLAGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion
                          -Wno-unused-parameter -Wno-unused-function)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "17.0")
        message(FATAL_ERROR "Clang 17+ required, found ${CMAKE_CXX_COMPILER_VERSION}")
    endif()
    # -Wno-unused-{parameter,function} on purpose: stubs / deferred-handler
    # surfaces are tracked in Dart with explicit follow-up tasks (Karpathy rule
    # 6 — surface signal, don't silence). Compile-time noise on those interferes
    # with -Werror gating on the warnings that DO catch real bugs.
    set(LCI_WARNING_FLAGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion
                          -Wno-unused-parameter -Wno-unused-function)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    # /wd4100 = unused parameter, /wd4505 = unused static function (match the
    # -Wno-unused-{parameter,function} rationale on GCC/Clang).
    set(LCI_WARNING_FLAGS /W4 /permissive- /wd4100 /wd4505)
endif()

option(LCI_WARNINGS_AS_ERRORS "Treat warnings as errors (for CI)" OFF)
if(LCI_WARNINGS_AS_ERRORS)
    if(MSVC)
        list(APPEND LCI_WARNING_FLAGS /WX)
    else()
        list(APPEND LCI_WARNING_FLAGS -Werror)
    endif()
endif()

# Version header generation
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/lci/version.h"
    @ONLY
)

# MCP JSON Schema embedding — schemas live as plain .json under share/lci/mcp-schemas/
# (human-reviewable, single source of truth) and are embedded into generated headers
# as C++ raw string literals so the binary has no runtime FS lookup. Mirrors Go's
# go:embed pattern. To add a schema: drop file in share/lci/mcp-schemas/, append to
# LCI_MCP_SCHEMAS, include the generated header (#include <lci/mcp/schemas/<name>.h>).
set(LCI_MCP_SCHEMAS search)
foreach(_schema IN LISTS LCI_MCP_SCHEMAS)
    set(_src "${CMAKE_CURRENT_SOURCE_DIR}/share/lci/mcp-schemas/${_schema}.json")
    set(_dst "${CMAKE_CURRENT_BINARY_DIR}/generated/lci/mcp/schemas/${_schema}.h")
    file(READ "${_src}" _schema_text)
    # Guard: raw string literal terminator R"LCI(...)LCI" — pick a delimiter no schema uses.
    string(TOUPPER "${_schema}" _schema_up)
    file(CONFIGURE
        OUTPUT "${_dst}"
        CONTENT "// Auto-generated from share/lci/mcp-schemas/${_schema}.json — DO NOT EDIT.\n// Edit the .json source; CMake regenerates on configure.\n#pragma once\nnamespace lci { namespace mcp { namespace schemas {\nconstexpr const char* k${_schema_up}_SCHEMA = R\"LCISCHEMA(${_schema_text})LCISCHEMA\";\n}}}\n"
        @ONLY
    )
endforeach()

# Dependencies via vcpkg (preferred) or FetchContent (fallback)
include(FetchContent)

find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    FetchContent_Declare(nlohmann_json
    SYSTEM
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

find_package(CLI11 CONFIG QUIET)
if(NOT CLI11_FOUND)
    FetchContent_Declare(cli11
    SYSTEM
        GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
        GIT_TAG v2.4.2
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(cli11)
endif()

find_package(httplib CONFIG QUIET)
if(NOT httplib_FOUND)
    FetchContent_Declare(httplib
    SYSTEM
        GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
        GIT_TAG v0.18.3
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(httplib)
endif()

# Raise the listen backlog from httplib's default of 5. The IndexServer accepts
# bursts of concurrent local clients (CLI fan-out, tests); a backlog of 5 drops
# connections under an 8+-connection burst (ECONNREFUSED -> intermittent
# ServerTest.ConcurrentRequests failures). 128 matches typical SOMAXCONN.
if(TARGET httplib)
    target_compile_definitions(httplib INTERFACE CPPHTTPLIB_LISTEN_BACKLOG=128)
elseif(TARGET httplib::httplib)
    target_compile_definitions(httplib::httplib INTERFACE CPPHTTPLIB_LISTEN_BACKLOG=128)
endif()

find_package(xxHash CONFIG QUIET)
if(NOT xxHash_FOUND)
    FetchContent_Declare(xxhash
    SYSTEM
        GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
        GIT_TAG v0.8.2
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR cmake_unofficial
    )
    FetchContent_MakeAvailable(xxhash)
endif()

find_package(absl CONFIG QUIET)
if(NOT absl_FOUND)
    set(ABSL_PROPAGATE_CXX_STD ON)
    set(ABSL_BUILD_TESTING OFF)
    FetchContent_Declare(abseil-cpp
    SYSTEM
        GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
        GIT_TAG 20240722.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(abseil-cpp)
endif()

# efsw — cross-platform file watcher (Linux inotify / macOS FSEvents / Windows ReadDirectoryChangesW)
# License: MIT. Replaces hand-rolled per-platform watcher in src/indexing/watcher.cpp.
find_package(efsw CONFIG QUIET)
if(NOT efsw_FOUND)
    set(VERBOSE OFF CACHE BOOL "" FORCE)
    set(BUILD_TEST_APP OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(efsw
    SYSTEM
        GIT_REPOSITORY https://github.com/SpartanJ/efsw.git
        GIT_TAG 1.4.1
        GIT_SHALLOW TRUE
        # Upstream data race (TSan-confirmed): FileWatcherInotify's destructor
        # spins `while (mIsTakingAction)` to hand off with its inotify worker
        # thread (which writes the flag in handleAction), but mIsTakingAction is
        # a plain bool — unsynchronised cross-thread access. efsw already wraps
        # mInitOK in its own Atomic<bool>; this makes mIsTakingAction match.
        # Applied via `cmake -P` (file READ/WRITE, not sed) so it is portable
        # across GNU/BSD/Windows and idempotent on an already-patched tree.
        PATCH_COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch-efsw.cmake
    )
    FetchContent_MakeAvailable(efsw)
endif()

# rapidfuzz-cpp — header-only SIMD-accelerated string similarity.
# License: MIT. Replaces hand-rolled Jaro-Winkler / Levenshtein / cosine
# in src/semantic/fuzzy_matcher.cpp. Matches the algorithm scale used by
# the rapidfuzz/Python ecosystem most users compare against.
find_package(rapidfuzz CONFIG QUIET)
if(NOT rapidfuzz_FOUND)
    FetchContent_Declare(rapidfuzz
    SYSTEM
        GIT_REPOSITORY https://github.com/rapidfuzz/rapidfuzz-cpp.git
        GIT_TAG v3.0.5
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(rapidfuzz)
endif()

# libstemmer (Snowball) — canonical Porter2 stemming algorithm.
# License: BSD-3-Clause. Replaces 449-LOC hand-port of Porter2 in
# src/semantic/stemmer.cpp. libstemmer == Go surgebase/porter2 fixture
# (29,417-word voc.txt/output.txt, used by both Snowball upstream and the
# surgebase Go test suite — see tests/data/porter2_fixture/).
#
# Karpathy: stemmer is on hot index/query path. Wrapper uses thread-local
# sb_stemmer (one per thread, reused across calls) + thread-local output
# buffer — zero alloc per token. See src/semantic/stemmer.cpp.
#
# Build mode: prefer vcpkg/system libstemmer; fall back to fetching the
# snowballstem/snowball-website libstemmer_c distribution and building an
# English-only static lib (avoid 30-language bloat). The distribution ships
# pre-generated C source so no Snowball compiler dependency is needed.
find_package(unofficial-libstemmer CONFIG QUIET)
if(unofficial-libstemmer_FOUND)
    # vcpkg target name
    set(LCI_LIBSTEMMER_TARGET unofficial::libstemmer::libstemmer)
elseif(EXISTS /usr/include/libstemmer.h)
    add_library(libstemmer_system INTERFACE)
    target_link_libraries(libstemmer_system INTERFACE stemmer)
    set(LCI_LIBSTEMMER_TARGET libstemmer_system)
else()
    FetchContent_Declare(libstemmer_c
    SYSTEM
        URL https://snowballstem.org/dist/libstemmer_c-2.2.0.tar.gz
        URL_HASH SHA256=b941d9fe9cf36b4e2f8d3873cd4d8b8775bd94867a1df8d8c001bb8b688377c3
    )
    FetchContent_MakeAvailable(libstemmer_c)
    # English-only build: runtime + UTF-8 English stemmer + custom thin
    # libstemmer_c.c that registers only english. Skips ~30 unused languages.
    set(LCI_LIBSTEMMER_SRC_DIR "${libstemmer_c_SOURCE_DIR}")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/libstemmer_english_only.c.in"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/libstemmer_english_only.c"
        @ONLY
    )
    add_library(libstemmer_lci STATIC
        "${LCI_LIBSTEMMER_SRC_DIR}/runtime/api.c"
        "${LCI_LIBSTEMMER_SRC_DIR}/runtime/utilities.c"
        "${LCI_LIBSTEMMER_SRC_DIR}/src_c/stem_UTF_8_english.c"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/libstemmer_english_only.c"
    )
    target_include_directories(libstemmer_lci PUBLIC
        "${LCI_LIBSTEMMER_SRC_DIR}/include"
    )
    target_include_directories(libstemmer_lci PRIVATE
        "${LCI_LIBSTEMMER_SRC_DIR}/runtime"
        "${LCI_LIBSTEMMER_SRC_DIR}"
    )
    # libstemmer C source predates strict warnings — silence to keep -Werror builds clean.
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        target_compile_options(libstemmer_lci PRIVATE
            -Wno-unused-parameter -Wno-unused-function
            -Wno-sign-compare -Wno-conversion -Wno-pedantic
        )
    endif()
    set(LCI_LIBSTEMMER_TARGET libstemmer_lci)
endif()

# re2 — Google's RE2 regex engine. License: BSD-3-Clause. Replaces std::regex
# in lci-cpp hot paths (regex_analyzer/engine, search.cpp regex_filter,
# semantic_annotator, vocabulary_analyzer, git/pattern_detector).
#
# Karpathy rule: std::regex is order-of-magnitude slower than RE2 on the search
# hot path. RE2 guarantees linear-time matching, has stable memory profile,
# and is reusable across threads when compiled once (immutable after ctor).
#
# Build mode: prefer vcpkg/system re2; fall back to FetchContent from the
# upstream Google repo at a pinned tag.
find_package(re2 CONFIG QUIET)
if(NOT re2_FOUND)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/AddRe2.cmake)
endif()

# nlohmann-json-schema-validator — JSON Schema draft-7/2019-09 validator built
# on nlohmann::json. License: MIT. Slated to replace hand-rolled MCP request
# validation (FIX-B). Dep wiring lives here; handler swap is a separate task.
find_package(nlohmann_json_schema_validator CONFIG QUIET)
if(NOT nlohmann_json_schema_validator_FOUND)
    set(JSON_VALIDATOR_INSTALL OFF CACHE BOOL "" FORCE)
    set(JSON_VALIDATOR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(JSON_VALIDATOR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(nlohmann_json_schema_validator
    SYSTEM
        GIT_REPOSITORY https://github.com/pboettch/json-schema-validator.git
        GIT_TAG 2.3.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json_schema_validator)
endif()

# tree-sitter is a C library.
#
# Always build from FetchContent at a pinned tag (v0.24.6). System / vcpkg
# tree-sitter is unreliable across distros: the pinned CI vcpkg baseline
# (01f602195) ships a stripped-down api.h that forward-declares TSNode as
# an opaque type, which breaks every TU using TSNode by value. Pinning to
# the upstream tag keeps Go-parity and Karpathy-rule hot paths deterministic.
FetchContent_Declare(tree-sitter
    SYSTEM
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter.git
    GIT_TAG v0.24.6
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(tree-sitter)
if(NOT tree-sitter_POPULATED)
    FetchContent_Populate(tree-sitter)
    add_library(tree-sitter STATIC
        "${tree-sitter_SOURCE_DIR}/lib/src/lib.c"
    )
    target_include_directories(tree-sitter PUBLIC
        "${tree-sitter_SOURCE_DIR}/lib/include"
    )
    target_include_directories(tree-sitter PRIVATE
        "${tree-sitter_SOURCE_DIR}/lib/src"
    )
endif()

# Tree-sitter language grammars (13 languages)
include(cmake/TreeSitterGrammars.cmake)
build_all_ts_grammars()

add_subdirectory(src)

enable_testing()
add_subdirectory(tests)

# ---------------------------------------------------------------------------
# CPack packaging (DEB, RPM, TGZ)
# ---------------------------------------------------------------------------
set(CPACK_PACKAGE_NAME "lci")
set(CPACK_PACKAGE_VENDOR "StandardBeagle")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Lightning Code Index - fast semantic code search for AI assistants")
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_PACKAGE_CONTACT "andy@standardbeagle.com")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
endif()
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/standardbeagle/lci")

install(TARGETS lci RUNTIME DESTINATION bin COMPONENT lci_runtime)

# FetchContent dependencies (httplib, absl, nlohmann, gtest, benchmark, etc.)
# unconditionally register their own install() rules. Restrict CPack to only
# the lci_runtime component so the package contains exactly bin/lci and no
# third-party headers or static libs. Both CPACK_COMPONENTS_ALL and the
# per-generator *_COMPONENT_INSTALL flags are required; the latter switches
# the generator out of "dump everything" mode.
set(CPACK_COMPONENTS_ALL lci_runtime)
set(CPACK_COMPONENT_LCI_RUNTIME_DESCRIPTION "Lightning Code Index binary")
set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_RPM_COMPONENT_INSTALL ON)

set(CPACK_GENERATOR "TGZ")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND CPACK_GENERATOR "DEB" "RPM")
endif()

# DEB-specific
set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6 (>= 2.35)")
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")

# RPM-specific
set(CPACK_RPM_PACKAGE_LICENSE "MIT")
set(CPACK_RPM_PACKAGE_GROUP "Development/Tools")
set(CPACK_RPM_PACKAGE_REQUIRES "glibc >= 2.35")

include(CPack)
