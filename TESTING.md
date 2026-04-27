# LCI C++ Port Testing Plan

## Phase 1: Build Verification

### 1.1 Clean Build (Release)
```bash
cd ~/work/core/lci-cpp
rm -rf build && cmake --preset release
cmake --build build/release -j$(nproc)
```
- [ ] Build completes with zero errors
- [ ] Binary produced at `build/release/lci`
- [ ] `./build/release/lci --version` prints version

### 1.2 Debug Build
```bash
cmake --preset debug
cmake --build build/debug -j$(nproc)
```
- [ ] Build completes with zero errors

### 1.3 Sanitizer Builds
```bash
# ASan + UBSan
cmake --preset sanitizer
cmake --build build/sanitizer -j$(nproc)

# TSan
cmake --preset tsan
cmake --build build/tsan -j$(nproc)
```
- [ ] Both sanitizer builds compile cleanly

---

## Phase 2: Automated Test Suite

### 2.1 Unit Tests
```bash
cd build/release
ctest -L unit --output-on-failure -j$(nproc)
```
- [ ] All unit tests pass (expect ~1,354+)
- [ ] Note any failures and their test names

### 2.2 Integration Tests
```bash
ctest -L integration --output-on-failure
```
- [ ] Pipeline integration tests pass (indexing real directories)
- [ ] Server lifecycle tests pass (all 15 endpoints)
- [ ] Search parity tests pass
- [ ] MCP tools integration tests pass

### 2.3 Infrastructure Tests
```bash
ctest -L infrastructure --output-on-failure
```
- [ ] IsolatedTestEnv cleanup works
- [ ] Concurrent helpers work
- [ ] Performance guards work

### 2.4 Sanitizer Test Runs
```bash
cd build/sanitizer
ctest --output-on-failure -j$(nproc)
```
- [ ] Zero ASan findings (no memory leaks, use-after-free, buffer overflow)
- [ ] Zero UBSan findings (no undefined behavior)

```bash
cd build/tsan
ctest --output-on-failure -j4
```
- [ ] Zero TSan findings (no data races)
- [ ] Note: some false positives possible with atomic operations

---

## Phase 3: CLI Parity Testing

Run each command and compare output format to the Go version. Use a real codebase (e.g., the lci repo itself).

### 3.1 Server Lifecycle
```bash
LCI=./build/release/lci

# Start server
$LCI server &
sleep 2

# Check status
$LCI status
```
- [ ] Server starts without errors
- [ ] Status shows "ready" with file count
- [ ] Unix socket created at expected path

### 3.2 Search Commands
```bash
# Basic search
$LCI search "MasterIndex"

# Grep mode
$LCI grep "TODO"

# JSON output
$LCI search --json "FileContent"

# Compact output
$LCI search --compact-search "trigram"

# Case insensitive
$LCI search -i "masterindex"

# Regex
$LCI search --regex "func.*Index"
```
- [ ] Each returns results
- [ ] JSON output is valid JSON
- [ ] Compare result count with Go `lci search` on same queries

### 3.3 Symbol Commands
```bash
# Definition lookup
$LCI def "MasterIndex"

# Reference lookup
$LCI refs "search"

# Call tree
$LCI tree "index_directory"

# List files
$LCI list

# Symbol listing
$LCI symbols --kind function

# Inspect
$LCI inspect "MasterIndex"

# Browse
$LCI browse src/indexing/master_index.cpp
```
- [ ] Each command returns sensible output
- [ ] Compare with Go output for same queries

### 3.4 Config Commands
```bash
$LCI config show
$LCI config validate
```
- [ ] Config show displays current settings
- [ ] Config validate reports no errors

### 3.5 Debug Commands
```bash
$LCI debug info
$LCI debug validate
```
- [ ] Debug info shows index statistics
- [ ] Validate reports consistency status

### 3.6 Shutdown
```bash
$LCI shutdown
```
- [ ] Server shuts down cleanly
- [ ] Socket file removed

---

## Phase 4: MCP Protocol Testing

### 4.1 MCP Startup
```bash
echo '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"test","version":"1.0"}}}' | $LCI mcp
```
- [ ] Returns valid JSON-RPC initialize response
- [ ] Lists 14 tools in capabilities

### 4.2 MCP Tool Listing
```bash
echo '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}' | $LCI mcp
```
- [ ] All 14 tools listed with schemas
- [ ] Tool names match Go version exactly:
  - `info`, `search`, `get_context`, `semantic_annotations`, `side_effects`, `code_insight`, `find_files`, `list_symbols`, `inspect_symbol`, `browse_file`, `index_stats`, `debug_info`, `git_analysis`, `context`

### 4.3 Claude Code Integration Test
Configure Claude Code to use the C++ binary as the LCI MCP server:
1. Update MCP config to point to `~/work/core/lci-cpp/build/release/lci mcp`
2. In Claude Code, try `/tools:search` with a query
3. Try `/tools:explore` on the codebase
- [ ] MCP server starts via Claude Code
- [ ] Search results return
- [ ] No crashes or hangs

---

## Phase 5: HTTP API Testing

With server running, use `curl` over Unix socket:

```bash
$LCI server &
sleep 2
SOCK=$(echo /tmp/lci-*.sock)

# Ping
curl --unix-socket $SOCK http://localhost/ping

# Status
curl --unix-socket $SOCK http://localhost/status

# Search
curl --unix-socket $SOCK -X POST http://localhost/search \
  -d '{"query":"MasterIndex","max_results":10}'

# Stats
curl --unix-socket $SOCK http://localhost/stats

# List symbols
curl --unix-socket $SOCK -X POST http://localhost/list-symbols \
  -d '{"max":5}'

# Definition
curl --unix-socket $SOCK -X POST http://localhost/definition \
  -d '{"symbol":"MasterIndex"}'

# References
curl --unix-socket $SOCK -X POST http://localhost/references \
  -d '{"symbol":"search"}'

# Tree
curl --unix-socket $SOCK -X POST http://localhost/tree \
  -d '{"symbol":"index_directory"}'

# File info
curl --unix-socket $SOCK -X POST http://localhost/fileinfo \
  -d '{"path":"src/indexing/master_index.cpp"}'

# Browse file
curl --unix-socket $SOCK -X POST http://localhost/browse-file \
  -d '{"path":"src/indexing/master_index.cpp"}'

# Inspect symbol
curl --unix-socket $SOCK -X POST http://localhost/inspect-symbol \
  -d '{"name":"MasterIndex"}'

# List symbols
curl --unix-socket $SOCK -X POST http://localhost/list-symbols \
  -d '{"max":10}'

# Reindex
curl --unix-socket $SOCK -X POST http://localhost/reindex

# Git analyze
curl --unix-socket $SOCK -X POST http://localhost/git-analyze \
  -d '{"scope":"wip"}'

# Shutdown
curl --unix-socket $SOCK -X POST http://localhost/shutdown
```
- [ ] Each endpoint returns valid JSON
- [ ] Response structure matches Go version
- [ ] No 500 errors

---

## Phase 6: Benchmarks

### 6.1 Run Google Benchmarks
```bash
cd build/release
./lci_benchmarks --benchmark_format=json > bench_results.json
```
- [ ] All 16 benchmarks complete
- [ ] Search latency < 1ms
- [ ] Trigram candidate search < 1us

### 6.2 Go vs C++ Comparison
```bash
cd ~/work/core/lci-cpp
LCI_CPP=./build/release/lci LCI_GO=$(which lci) \
  bash scripts/benchmark-compare.sh
```
- [ ] Script runs to completion
- [ ] Record results:

| Metric | Go | C++ | Winner |
|--------|----|-----|--------|
| Startup time | | | |
| Index 1k files | | | |
| Search latency p50 | | | |
| Search latency p99 | | | |
| Memory RSS (idle) | | | |
| Memory RSS (indexing) | | | |

---

## Phase 7: Stress and Edge Cases

### 7.1 Large Codebase
Index a large codebase (e.g., Linux kernel subset, or node_modules):
```bash
$LCI server -r /path/to/large/codebase &
$LCI status  # wait for indexing to complete
$LCI search "main"
```
- [ ] Indexing completes without OOM
- [ ] Search returns results
- [ ] Memory usage reasonable (`ps aux | grep lci`)

### 7.2 Concurrent Access
Run multiple searches in parallel:
```bash
for i in $(seq 1 20); do
  $LCI search "function_$i" &
done
wait
```
- [ ] All searches complete
- [ ] No crashes or deadlocks

### 7.3 Rapid File Changes (Watcher)
With server running, rapidly create/modify/delete files:
```bash
mkdir -p /tmp/lci-watch-test
$LCI server -r /tmp/lci-watch-test &
sleep 2
for i in $(seq 1 50); do
  echo "func test_$i() {}" > /tmp/lci-watch-test/test_$i.go
  sleep 0.05
done
sleep 1
$LCI status
```
- [ ] Watcher debounces correctly
- [ ] No crashes from rapid changes
- [ ] Status shows files were picked up

### 7.4 Empty and Invalid Inputs
```bash
$LCI search ""
$LCI search "$(python3 -c 'print("A"*10000)')"
$LCI def ""
$LCI browse nonexistent_file.cpp
$LCI search --json "{"
```
- [ ] Graceful error messages (no crashes, no panics)

---

## Phase 8: Known Issues to Verify

From loop observations during the port:

- [ ] **Pipeline race condition**: Index 3+ files concurrently, search immediately. Verify results are complete. Known intermittent issue with parallel file processing.
- [ ] **AdaptivePaginator.TokenTruncation**: Check if this test is still flaky.
- [ ] **8 pre-existing test failures**: Investigate CLI config and server lifecycle test failures. Document root causes.
- [ ] **std::regex vs RE2**: Regex engine uses `std::regex` instead of RE2. Test complex regex patterns for correctness and performance.

---

## Test Results

| Phase | Test | Pass/Fail | Notes |
|-------|------|-----------|-------|
| 1.1 | Release build | | |
| 1.2 | Debug build | | |
| 1.3 | Sanitizer builds | | |
| 2.1 | Unit tests (count: ___) | | |
| 2.2 | Integration tests | | |
| 2.3 | Infrastructure tests | | |
| 2.4 | ASan clean | | |
| 2.4 | TSan clean | | |
| 3.1 | Server lifecycle | | |
| 3.2 | Search commands (6 variants) | | |
| 3.3 | Symbol commands (7 variants) | | |
| 3.4 | Config commands | | |
| 3.5 | Debug commands | | |
| 3.6 | Shutdown | | |
| 4.1 | MCP startup | | |
| 4.2 | MCP tool listing | | |
| 4.3 | Claude Code integration | | |
| 5 | HTTP endpoints (15) | | |
| 6.1 | Google Benchmarks | | |
| 6.2 | Go vs C++ comparison | | |
| 7.1 | Large codebase | | |
| 7.2 | Concurrent access | | |
| 7.3 | Rapid file changes | | |
| 7.4 | Invalid inputs | | |
| 8 | Known issues checked | | |
