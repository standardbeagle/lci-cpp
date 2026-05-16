cmake_minimum_required(VERSION 3.25)

project(lci
    VERSION 0.1.0
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

set(LCI_GO_PATH "" CACHE FILEPATH "Path to Go reference lci binary for parity tests")

# Compiler detection and flags
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "13.0")
        message(FATAL_ERROR "GCC 13+ required, found ${CMAKE_CXX_COMPILER_VERSION}")
    endif()
    add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    # Apple Clang uses its own version numbering (e.g. 15.x ships with C++20 support)
    add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "17.0")
        message(FATAL_ERROR "Clang 17+ required, found ${CMAKE_CXX_COMPILER_VERSION}")
    endif()
    add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    add_compile_options(/W4 /permissive-)
endif()

option(LCI_WARNINGS_AS_ERRORS "Treat warnings as errors (for CI)" OFF)
if(LCI_WARNINGS_AS_ERRORS)
    if(MSVC)
        add_compile_options(/WX)
    else()
        add_compile_options(-Werror)
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
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

find_package(CLI11 CONFIG QUIET)
if(NOT CLI11_FOUND)
    FetchContent_Declare(cli11
        GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
        GIT_TAG v2.4.2
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(cli11)
endif()

find_package(httplib CONFIG QUIET)
if(NOT httplib_FOUND)
    FetchContent_Declare(httplib
        GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
        GIT_TAG v0.18.3
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(httplib)
endif()

find_package(xxHash CONFIG QUIET)
if(NOT xxHash_FOUND)
    FetchContent_Declare(xxhash
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
        GIT_REPOSITORY https://github.com/SpartanJ/efsw.git
        GIT_TAG 1.4.1
        GIT_SHALLOW TRUE
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

# nlohmann-json-schema-validator — JSON Schema draft-7/2019-09 validator built
# on nlohmann::json. License: MIT. Slated to replace hand-rolled MCP request
# validation (FIX-B). Dep wiring lives here; handler swap is a separate task.
find_package(nlohmann_json_schema_validator CONFIG QUIET)
if(NOT nlohmann_json_schema_validator_FOUND)
    set(JSON_VALIDATOR_INSTALL OFF CACHE BOOL "" FORCE)
    set(JSON_VALIDATOR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(JSON_VALIDATOR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(nlohmann_json_schema_validator
        GIT_REPOSITORY https://github.com/pboettch/json-schema-validator.git
        GIT_TAG 2.3.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json_schema_validator)
endif()

# tree-sitter is a C library
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(TREE_SITTER QUIET tree-sitter)
endif()
if(NOT TREE_SITTER_FOUND)
    FetchContent_Declare(tree-sitter
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

install(TARGETS lci RUNTIME DESTINATION bin)

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
