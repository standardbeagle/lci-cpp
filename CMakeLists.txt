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
