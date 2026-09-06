# Edit-task answer keys (oracle patches, probes, evidence)

This directory holds the ANSWER KEYS for the stage-3 edit tasks declared in
`../tasks/`. Nothing here is agent-visible: `validate_edit_tasks.py` fails any
task whose prompt leaks a patch path or a substantial patch content line, and
whose `behavior.command` names a patch target relpath.

## Layout

```
patches/<corpus>/<task-id>.json                 oracle patch sidecar
patches/<corpus>/<task-id>.probe.{go,py,js}     discrimination probe
patches/<corpus>/evidence/<task-id>.oracle.json captured oracle outcome
```

`<corpus>` is the corpus dir name (`pocketbase`, `scikit-learn`, `next.js`);
`<task-id>` is the task's `id`.

### Oracle patch sidecar (`<task-id>.json`)

```json
{
  "schema": "edit_oracle_patch_v1",
  "task_id": "pb-api-1",
  "files": {
    "apis/handler.go": "<full post-edit file contents>",
    "apis/dead_twin.go": null
  }
}
```

`files` maps tree-relative paths to their FULL post-edit contents; `null`
deletes the file. `oracle_gate.load_oracle_patch_file` loads it and
`apply_patch` applies it to a throwaway copy of the pristine tree. The task
JSON references it via `oracle_patch.path = "patches/<corpus>/<task-id>.json"`.

### Discrimination probe (`<task-id>.probe.<ext>`)

The task's `behavior.command` runs the probe with cwd = the materialized tree:

```json
"behavior": {"command": ["go", "run", "{edits_root}/patches/pocketbase/pb-api-1.probe.go"]}
```

`{edits_root}` and `{python}` are substituted by `oracle_gate.resolve_argv`.
The argv names ONLY the task id; the target path lives in the probe body,
never in the task JSON (conformance tests read task JSON).

### Evidence (`evidence/<task-id>.oracle.json`)

The verbatim `oracle_gate_v1` outcome captured by
`scripts/run_edit_oracles.py --evidence-dir`. It is committed as EVIDENCE that
the answer key discriminated when authored — it is NOT a byte-compared golden:
captured output tails and environments drift, so nothing diff-compares it.

## Decisions

- **D1 — `behavior.command` is the probe argv, not the repo's test suite.**
  No generic suite exit code changes on a convention fix (recon comment
  01KXQ08QB52RJ820DY7HP1WAQ2), so a generic suite cannot discriminate. The
  corpus's DECLARED existing suite moves to `existing_suite.command`, where
  the gate runs it on the patched tree as a regression guard only.
- **D2 — a probe is a COMPILER/AST-BACKED structural assertion.** `go/parser`
  via `go run`, Python's `ast`, or the TypeScript compiler API via `node`.
  Never a regex, and never `conformance_gate.py` or the task's
  `conforms_pattern`: per the bench-harness-oracle-independence rule 1, the
  oracle's matcher must be independent of the machinery it cross-checks, or
  both share the same blind spot.
- **D3 — a probe exits non-zero on the pristine tree and zero on the patched
  tree.** `evaluate_oracle`'s DISCRIMINATES verdict IS the probe's
  discrimination test (rule 2, both directions): red→green passes;
  green→green, red→red, and green→red are all hard rejects. Do not write a
  separate discrimination harness.
- **D4 — patch files are full post-edit file contents, not diffs**, at most
  `blast_radius.max_files` entries, every path matching a
  `blast_radius.allow` glob. The bank validator enforces this with `fnmatch`,
  deliberately independent of the gate's own glob translator.
