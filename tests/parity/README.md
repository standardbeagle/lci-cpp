# LCI Parity Harness

Side-by-side verification of the C++ `lci` port against the Go reference.

**Current baseline: 11 / 55 descriptors passing.** See
[`KNOWN_FAILURES.md`](KNOWN_FAILURES.md) for the failure list, dominant causes
per category, and recommended fix order. Performance comparison is meaningless
until correctness parity is restored.

## Run

    cmake --preset debug
    cmake --build build/debug -j$(nproc)
    ctest --test-dir build/debug -L parity --output-on-failure -j$(nproc)

## Triage failures

When a parity test fails, the runner writes a dump to
`build/debug/parity-failures/<test_id>/`:

| File | Contents |
|------|----------|
| `desc.json` | the descriptor used |
| `go.raw`, `cpp.raw` | uninterpreted captured output from each binary |
| `go.canon.json`, `cpp.canon.json` | post-canonicalize structures |
| `diff.txt` | unified diff of canon |
| `report.txt` | per-tier reason breakdown |

See `docs/superpowers/specs/2026-04-27-lci-cpp-vs-go-parity-verification-design.md` for tier semantics.

---

## Adding a new descriptor

Pick a mode (`cli`, `mcp`, `http`, or `index`) and copy the closest existing descriptor from `descriptors/<mode>/`. 
Update the following fields in your copy:

- `id` — unique identifier (e.g., `cli/search/custom` or `http/my-endpoint`)
- `args` — command-line arguments (for cli) or endpoint path + stdin (for http/mcp)
- `corpus` — test corpus name (e.g., `multi-lang`, `empty`, `lci-go-repo`)
- `tiers` — define `stable` (must match exactly), `ranked` (score tolerance), `timed` (timing tolerance), and `ignore` (skip these paths)

Build and run only the new test:

```bash
cmake --build build/debug -j$(nproc)
ctest --test-dir build/debug -L parity -R 'parity\.<id>' --output-on-failure
```

### Interpreting descriptor tiers

- **`stable`** — paths that must match exactly across both binaries. Any divergence is a bug.
- **`ranked`** — scoring fields (e.g., `results[].score`) where relative ordering matters but absolute values can drift within `tolerances.score_abs`.
- **`timed`** — fields like `elapsed_ms` where values must fall within `[0, tolerances.timed_max_ms]`.
- **`ignore`** — paths to skip entirely (e.g., PIDs, timestamps, non-deterministic ordering).

If the test fails, inspect the failure dump at `build/debug/parity-failures/<id>/`:

1. Open `report.txt` for the summary of which tier caused the failure.
2. Open `diff.txt` for the unified diff of the canonicalized output.
3. Decide if the divergence is a **bug** (structural mismatch in logic) or an **intentional difference** (new C++ field, schema drift).

Adjust `tiers` ONLY for intentional divergences:
- New C++ field? Add the path to `ignore` with a brief comment (e.g., `// C++ extension: faster I/O tracking`).
- Non-deterministic ordering that's valid? Move from `stable` to `ignore`.
- Scoring algorithm improved in C++? Move from `stable` to `ranked` if acceptable.

Never silence a real bug by adjusting tiers — file it instead (see "Known divergences" in `MODULE_MAP.md`).

---

## Triaging a parity failure

When a test fails, the harness writes a detailed report. Follow this workflow:

1. **Open `build/debug/parity-failures/<test_id>/report.txt`**  
   Check which tier(s) reported the divergence. The report format is:
   ```
   STABLE: 3 paths diverged
   RANKED: score within tolerance
   TIMED: elapsed_ms = 1234 (in range [0, 60000])
   ```

2. **Open `build/debug/parity-failures/<test_id>/diff.txt`**  
   Review the unified diff of the canonicalized output:
   ```diff
   --- go.canon.json
   +++ cpp.canon.json
   @@ -1,5 +1,5 @@
    {
   -  "count": 42,
   +  "count": 41,
   ```
   This diff shows exactly which fields changed.

3. **Classify the divergence:**

   - **STABLE divergence** = a required field differs when it should match.  
     This is a bug. File it in `MODULE_MAP.md` under "Known divergences" with:
     - Test ID
     - Field path (e.g., `symbols[0].name`)
     - Expected (Go) vs. actual (C++)
     - Likely cause (parser gap, symbol linker bug, etc.)

   - **RANKED beyond `score_abs`** = a scoring/ranking field drifted too far.  
     Investigate the semantic scorer or ranking logic. May indicate a real bug or a legitimate scoring improvement.

   - **TIMED out of `[0, timed_max_ms]`** = execution took longer than tolerance.  
     This indicates a performance regression. Profile the slow operation and fix if it's a real bottleneck.

4. **For intentional divergences:**  
   If the divergence is by design (e.g., a new C++ optimization field that Go doesn't have):
   - Add the path to the descriptor's `ignore` list.
   - Include a brief comment explaining why (e.g., `// C++ extension: caching metadata`).
   - Re-run the test to confirm it passes.
   - Document the divergence in `MODULE_MAP.md` "Known divergences" as approved (Phase 1, Phase 2, etc.).

---

## Adding a new algorithmic probe

**Probe** = a small, deterministic `lci debug <subcommand>` that exists on BOTH binaries with consistent output.

### Example: adding `lci debug score <query> <id>`

1. Ensure both `lci-go` and `lci-cpp` support the subcommand:
   ```bash
   $LCI_GO debug score "myQuery" "12345" 2>&1
   $LCI_CPP debug score "myQuery" "12345" 2>&1
   ```
   Both should succeed or both should fail in the same way.

2. Create a new descriptor at `descriptors/probes/score.parity.json`:
   ```json
   {
     "id": "probes/score",
     "mode": "cli",
     "corpus": "multi-lang",
     "go_binary": "${LCI_GO}",
     "cpp_binary": "${LCI_CPP}",
     "invocation": {
       "args": ["debug", "score", "myQuery", "12345"],
       "cwd": "${CORPUS}"
     },
     "capture": ["stdout", "exit"],
     "parse": "text",
     "tiers": {},
     "tolerances": {"score_abs": 0.0, "timed_max_ms": 60000},
     "expect_exit": 0
   }
   ```

3. Update `MODULE_MAP.md` "Backlog: probes to add" — check the row for the new subcommand:
   ```markdown
   - [x] `lci debug score <query> <id>` — go: YES, cpp: YES — descriptor added
   ```

---

## Mode-specific notes

### CLI mode

**Standard fork+exec subprocess model.**

- `${CORPUS}` is substituted into `invocation.args`, `invocation.env`, and `invocation.cwd`.
- Binary is executed in the corpus directory (cwd).
- `stdout` and `stderr` are drained concurrently to avoid pipe-buffer deadlock.
- Exit code is captured and compared against `expect_exit`.

**Example:**
```json
{
  "id": "cli/search/basic",
  "mode": "cli",
  "corpus": "multi-lang",
  "invocation": {
    "args": ["search", "add"],
    "cwd": "${CORPUS}"
  },
  "capture": ["stdout", "exit"],
  "parse": "text"
}
```

### MCP mode

**Two wire-protocol framings supported:**

- **Go**: newline-delimited JSON (ndjson) — one JSON object per line.
- **C++**: LSP-style Content-Length headers — `Content-Length: <N>\r\n\r\n<N bytes of JSON>`.

The runner automatically detects which framing to use based on the environment variables:
- If `LCI_GO` matches, use ndjson.
- If `LCI_CPP` matches, use Content-Length headers.

**Handshake sequence:**
1. Binary starts; runner sends `initialize` message.
2. Wait for `notifications/initialized` response.
3. Send tool call (e.g., `tools/call` with method name and arguments).
4. Capture response and any subsequent notifications.

**Protocol version:** `2025-06-18`

**Example:**
```json
{
  "id": "mcp/search/basic",
  "mode": "mcp",
  "corpus": "multi-lang",
  "invocation": {
    "args": ["mcp"],
    "stdin": "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"search\",\"arguments\":{\"pattern\":\"add\"}}}"
  },
  "capture": ["stdout", "exit"],
  "parse": "json"
}
```

### HTTP mode

**Both binaries compute their unix socket path from the corpus path:**
```
socket_path = /tmp/lci-server-<hash>.sock
```

where `<hash>` is derived from the absolute corpus path using an identical hash algorithm. This ensures both binaries use the same socket.

**Server lifecycle:**
- Servers run sequentially (not concurrently).
- Runner waits up to 10 seconds for the socket to appear and `/status` to respond with `ready=true`.
- Request is sent to the socket via HTTP (treated as localhost despite socket-path Host).
- Response is captured.

**Important:** The Go server may reject a raw socket-path Host header (e.g., `Host: /tmp/lci-server-xyz.sock`).  
The runner overrides this to `Host: localhost` for compatibility.

**Example:**
```json
{
  "id": "http/search",
  "mode": "http",
  "corpus": "multi-lang",
  "invocation": {
    "args": ["/search"],
    "stdin": "{\"pattern\":\"add\"}"
  },
  "capture": ["stdout", "exit"],
  "parse": "json",
  "tolerances": {"score_abs": 0.05, "timed_max_ms": 60000}
}
```

### Index mode

**JSON export parity for the full symbol+reference graph.**

The runner invokes each binary with:
```bash
lci debug export --output=<tmpfile>
```

(Note: The `--json` flag does not exist on either binary; use `--output` to write JSON to a file.)

Both files are read, parsed as JSON, and compared according to the descriptor's tier definitions.

**Example:**
```json
{
  "id": "index/synthetic-multilang",
  "mode": "index",
  "corpus": "multi-lang",
  "invocation": {
    "args": [],
    "cwd": "${CORPUS}"
  },
  "capture": ["stdout", "exit"],
  "parse": "json",
  "tiers": {
    "stable": [
      "files[].path",
      "files[].language",
      "symbols[].name",
      "symbols[].kind"
    ]
  }
}
```

---

## References

- **Parity verification design:**  
  `docs/superpowers/specs/2026-04-27-lci-cpp-vs-go-parity-verification-design.md`

- **Module mapping & backlog:**  
  `tests/parity/MODULE_MAP.md`

- **Descriptor schema:**  
  Each `*.parity.json` file in `descriptors/<mode>/`
