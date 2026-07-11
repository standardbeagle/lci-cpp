# Correctness and Refactoring Design

## Goal

Fix the three confirmed defects, then reduce the duplication and oversized
components that made those defects difficult to prevent. Every phase must leave
the tree buildable, tested, and independently reversible.

## Bug-fix phase

REST result limits will pass through one bounded-integer helper before reaching
search code or container sizing operations. Nonpositive values use the endpoint
default, and excessive values use a documented endpoint cap. Regression tests
will send negative and excessive limits to the affected endpoints.

The KDL lexer will distinguish terminated strings and block comments from input
that ends prematurely. The parser will require a closing brace for every opened
block. Errors will retain the current line-oriented diagnostics. Tests will
cover each truncated construct and a well-formed neighboring case.

On POSIX, detached spawning will use a short-lived intermediate process. The
caller will reap that intermediate process synchronously; the spawned daemon
will be reparented and therefore cannot remain as the caller's zombie child.
The intermediate process will report whether `posix_spawnp` succeeded through
its exit status, preserving the existing API contract.

## Refactoring phases

1. Consolidate repeated ASCII case-folding and comparison helpers.
2. Decode REST JSON into typed requests whose constructors enforce bounds.
3. Move server and MCP endpoint implementations into domain-focused files.
4. Keep AST traversal in `UnifiedExtractor`, but dispatch language behavior to
   language-focused collaborators behind a narrow extraction context.
5. Remove the unused paginator or connect it to an actual page/cursor contract.

Each phase starts with characterization tests. Mechanical moves precede behavior
changes, and each phase gets its own commit.

## Verification gates

For every commit: build with project warnings enabled, run the directly affected
GoogleTest filters, and inspect the diff. After each refactoring phase, run the
full unit executable. At the end, run CTest's unit and integration labels. Unix
socket tests that cannot bind in a restricted environment must be rerun in an
environment that permits AF_UNIX sockets; unrelated failures will not be treated
as successful verification.
