# Tree-sitter language grammar declarations.
# Each grammar is fetched and built as a static library.

include(FetchContent)

# Helper: declare a tree-sitter grammar from a GitHub repo.
# Each grammar repo provides src/parser.c (required) and optionally
# src/scanner.c or src/scanner.cc.
function(declare_ts_grammar NAME REPO TAG)
    FetchContent_Declare(tree-sitter-${NAME}
        GIT_REPOSITORY ${REPO}
        GIT_TAG ${TAG}
        GIT_SHALLOW TRUE
    )
endfunction()

# Declare all 13 language grammars.
declare_ts_grammar(go
    https://github.com/tree-sitter/tree-sitter-go.git v0.23.4)
declare_ts_grammar(python
    https://github.com/tree-sitter/tree-sitter-python.git v0.23.6)
declare_ts_grammar(javascript
    https://github.com/tree-sitter/tree-sitter-javascript.git v0.23.1)
declare_ts_grammar(typescript
    https://github.com/tree-sitter/tree-sitter-typescript.git v0.23.2)
declare_ts_grammar(rust
    https://github.com/tree-sitter/tree-sitter-rust.git v0.23.2)
declare_ts_grammar(c
    https://github.com/tree-sitter/tree-sitter-c.git v0.23.4)
declare_ts_grammar(cpp
    https://github.com/tree-sitter/tree-sitter-cpp.git v0.23.4)
declare_ts_grammar(java
    https://github.com/tree-sitter/tree-sitter-java.git v0.23.5)
declare_ts_grammar(c-sharp
    https://github.com/tree-sitter/tree-sitter-c-sharp.git v0.23.1)
declare_ts_grammar(php
    https://github.com/tree-sitter/tree-sitter-php.git v0.23.11)
declare_ts_grammar(kotlin
    https://github.com/fwcd/tree-sitter-kotlin.git 0.3.8)
declare_ts_grammar(zig
    https://github.com/tree-sitter-grammars/tree-sitter-zig.git v1.1.1)
declare_ts_grammar(ruby
    https://github.com/tree-sitter/tree-sitter-ruby.git v0.23.1)

# Build helper: populate a grammar and create a static library target.
# scanner_type: "none", "c", or "cc" for scanner language.
function(build_ts_grammar NAME SCANNER_TYPE)
    FetchContent_GetProperties(tree-sitter-${NAME})
    if(NOT tree-sitter-${NAME}_POPULATED)
        FetchContent_Populate(tree-sitter-${NAME})
    endif()

    set(_src_dir "${tree-sitter-${NAME}_SOURCE_DIR}/src")

    # Every grammar has parser.c
    set(_sources "${_src_dir}/parser.c")

    # Add scanner if present
    if(SCANNER_TYPE STREQUAL "c")
        list(APPEND _sources "${_src_dir}/scanner.c")
    elseif(SCANNER_TYPE STREQUAL "cc")
        list(APPEND _sources "${_src_dir}/scanner.cc")
    endif()

    add_library(tree-sitter-${NAME} STATIC ${_sources})
    target_include_directories(tree-sitter-${NAME} PUBLIC
        "${tree-sitter-${NAME}_SOURCE_DIR}/src"
    )
    # Grammars need the tree-sitter header for TSLanguage
    target_link_libraries(tree-sitter-${NAME} PRIVATE tree-sitter)

    # Suppress warnings in generated grammar code
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
        target_compile_options(tree-sitter-${NAME} PRIVATE -w)
    elseif(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(tree-sitter-${NAME} PRIVATE /w)
    endif()
endfunction()

# TypeScript grammar is special: it has two sub-languages (typescript and tsx)
# in separate directories under the repo.
function(build_ts_grammar_typescript)
    FetchContent_GetProperties(tree-sitter-typescript)
    if(NOT tree-sitter-typescript_POPULATED)
        FetchContent_Populate(tree-sitter-typescript)
    endif()

    set(_base "${tree-sitter-typescript_SOURCE_DIR}")

    # TypeScript variant
    add_library(tree-sitter-typescript STATIC
        "${_base}/typescript/src/parser.c"
        "${_base}/typescript/src/scanner.c"
    )
    target_include_directories(tree-sitter-typescript PUBLIC
        "${_base}/typescript/src"
    )
    target_link_libraries(tree-sitter-typescript PRIVATE tree-sitter)

    # TSX variant
    add_library(tree-sitter-tsx STATIC
        "${_base}/tsx/src/parser.c"
        "${_base}/tsx/src/scanner.c"
    )
    target_include_directories(tree-sitter-tsx PUBLIC
        "${_base}/tsx/src"
    )
    target_link_libraries(tree-sitter-tsx PRIVATE tree-sitter)

    # Suppress warnings
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
        target_compile_options(tree-sitter-typescript PRIVATE -w)
        target_compile_options(tree-sitter-tsx PRIVATE -w)
    elseif(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(tree-sitter-typescript PRIVATE /w)
        target_compile_options(tree-sitter-tsx PRIVATE /w)
    endif()
endfunction()

# PHP grammar has php and php_only sub-languages similar to TypeScript
function(build_ts_grammar_php)
    FetchContent_GetProperties(tree-sitter-php)
    if(NOT tree-sitter-php_POPULATED)
        FetchContent_Populate(tree-sitter-php)
    endif()

    set(_base "${tree-sitter-php_SOURCE_DIR}")

    # Check if it has subdirectories (newer versions) or flat structure
    if(EXISTS "${_base}/php/src/parser.c")
        add_library(tree-sitter-php STATIC
            "${_base}/php/src/parser.c"
            "${_base}/php/src/scanner.c"
        )
        target_include_directories(tree-sitter-php PUBLIC
            "${_base}/php/src"
        )
    else()
        add_library(tree-sitter-php STATIC
            "${_base}/src/parser.c"
            "${_base}/src/scanner.c"
        )
        target_include_directories(tree-sitter-php PUBLIC
            "${_base}/src"
        )
    endif()
    target_link_libraries(tree-sitter-php PRIVATE tree-sitter)

    if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
        target_compile_options(tree-sitter-php PRIVATE -w)
    elseif(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(tree-sitter-php PRIVATE /w)
    endif()
endfunction()

# Build all grammars.
function(build_all_ts_grammars)
    build_ts_grammar(go none)
    build_ts_grammar(python "c")
    build_ts_grammar(javascript "c")
    build_ts_grammar_typescript()
    build_ts_grammar(rust "c")
    build_ts_grammar(c none)
    build_ts_grammar(cpp "c")
    build_ts_grammar(java none)
    build_ts_grammar(c-sharp "c")
    build_ts_grammar_php()
    build_ts_grammar(kotlin "c")
    build_ts_grammar(zig none)
    build_ts_grammar(ruby "c")
endfunction()
