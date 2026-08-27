# Contamination audit: parent-repo access from bench cells (2026-08-27)

Trigger: the naming-hard run exposed opencode_runner leaking the parent
shell's $PWD into "isolated" cells (bun trusts $PWD over the real cwd), which
made every naming-hard cell run in the PARENT lci-cpp repo — one cell read
the bench's own gold-answer file. Fixed in opencode_runner (PWD pinned to the
workspace). This audit sweeps ALL prior recorded results for the same class.

## Method

Two sweeps over benchmarks/repo-qa/results (2,621 records with tool traces):
1. Content tells that can only come from the parent repo (lci-cpp/src|include,
   AGENTS.md content, worktrack, internal file names) in answers.
2. Tool-trace scan: COMPLETED tool calls whose args reference lci-cpp outside
   benchmarks/repo-qa/.work, split by substantive output (>100 chars) vs
   no-match/error (misdirection only, costs tokens but injects nothing).

## Findings

- 17 cells (0.6%) had substantive parent-repo reads; 39 total completed
  parent-repo calls; the rest of the flagged calls returned errors or empty
  matches (misdirection cost only, answers uncontaminated).
- Worst class: tier2 ripgrep gpt5mini cells answered from
  real_projects/rust/ripgrep — a DIFFERENT ripgrep checkout that ships
  inside this repo as a test corpus. Same class for one guzzle cell
  (grep over the whole parent tree).
- Cluster: base-arm zls/okhttp cells (northmini, mimo, goglm52, zhipuglm52)
  wandered to the parent root via find/grep when lost.
- These are per-cell defects, not per-suite: LCI-arm cells were almost
  untouched (tool access goes through the MCP, not filesystem paths).

Affected cells (substantive reads, count of such calls):
  discovery{,-fixed}: okhttp__mimo base d1 (1), zls__goglm52 base d2 (1),
    zls__northmini base d1 (2), zls__zhipuglm52 base d2 (1)
  tier2: guzzle__gpt5mini base e3 (1), ripgrep__gpt5mini base e2 (3),
    ripgrep__gpt5mini base h3 (1)
  tier3{,-fixed}: okhttp__northmini base m3 (1), zls__mimo base h2 (1),
    zls__northmini base h1 (2)

## Disposition

1. The 17 cells are quarantined: exclude them from any future aggregate
   citing discovery/tier2/tier3 numbers. Since the contamination is in
   BASE-arm cells and mostly wasted effort or wrong-checkout reads, prior
   base-arm scores are, if anything, slightly optimistic-or-noisy; no
   conclusion in the ANALYSIS-* files flips on 17/2621 cells, but re-runs
   (already owed per repo-qa memory: all pre-3abebc2 tiers ran crippled LCI)
   should use the fixed runner.
2. Root-cause fixes already landed: PWD pinned (opencode_runner),
   step-finish token parsing. Future runs also inherit the naming-hard
   workspace pattern: corpus copied OUT of the parent tree entirely would
   be stronger — .work/ lives inside the repo, so a lost model can climb
   with a bare find. Follow-up worth doing before the next paid sweep:
   move run workspaces to /tmp (naming_hard_run already does).
3. real_projects/ (stale corpus copies inside the repo) is an attractive
   nuisance: any lost base-arm model finds a plausible-but-wrong checkout.
   Candidate: exclude real_projects/ from bench workspaces or delete from
   the repo if the real-project suite no longer needs it in-tree.
