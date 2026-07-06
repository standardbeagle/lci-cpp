# git-analyze fixture

Committed source for the `cli/git/git-analyze` integration spec. The runner
materializes this directory into a fresh temp git repo (init + add + commit)
so the WIP-scope analysis runs against a guaranteed-clean working tree,
independent of any live corpus's mutable state.
