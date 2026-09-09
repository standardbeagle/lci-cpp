# git-analyze fixture

Committed source for the `cli/git/git-analyze` integration spec. The runner
materializes this directory into a fresh temp git repo (init + add + commit)
so the WIP-scope analysis runs against a guaranteed-clean working tree,
independent of any live corpus's mutable state.

Also used by the `*-commit` specs (`cli/git/git-analyze-commit`,
`http/git-analyze-commit`, `mcp/git_analysis/commit-finding`), which run
`--scope commit` (default base_ref HEAD) against this same fixture's single
root commit so `diff-tree --root` reads the whole tree as Added and the
analyzer produces real findings instead of an empty diff:

- `huge.go` defines a 130-line function, unconditionally past the fixed
  long_function threshold (100 lines) — a real metrics finding.
- `duplicate.go` defines two functions with identical bodies and different
  names (`ComputeTotal` / `ComputeTotalTwo`) — a real structural duplicate
  finding (token-set similarity above the 0.8 default threshold).
