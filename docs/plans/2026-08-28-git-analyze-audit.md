# git-analyze audit — verdict and remaining defects (2026-08-28)

Requested gate before building the enhanced-names computed-qualification
tier: are the git changeset tools working and worthwhile?

## Verdict

- **git_hotspots: working, worthwhile, keep.** 14s on a 353-commit window;
  per-file changes/authors/churn plus collision scores are the
  conflict-likelihood signal NEXT STEPS points agents at. Only nit: no
  caching — every call replays the window.
- **git-analyze: worthwhile detector, broken delivery — partially fixed.**
  The duplicate detector finds real things (probe commit surfaced
  is_function_like copy-pasted across four analyzers, 100% similarity).
  Fixed this audit (f660a17): CLI 30s-timeout death, findings rendered in
  LCF, gtest-macro naming noise. Still defective, in priority order below.

## Update (same day): defects 1+4 fixed via ScopeSet

lci::ScopeSet (analysis/scope_set.h) is the general folder/file/element
selection abstraction: path -> merged line ranges, set algebra, and
populators for paths, globs, regex, code-model query results
(scope_from_symbols), and unified-diff hunks (scope_from_unified_diff /
Provider::get_changed_scope). git-analyze now intersects parsed symbols
with the diff's new-side hunks (Added files stay whole-file; a failed
hunk fetch falls back to unscoped — over-report, never drop). Probe
commit f7180a0: modified 100 -> 2, findings 27 -> 2 (both change-
relevant), risk 1.00 -> 0.47, analysis 42s -> 7.6s. Known residue: the
index reflects the working tree, so line drift between an analyzed
historic commit and today's tree can make a symbol "duplicate" its own
shifted copy (self-match guard compares exact (path,line)).

## Update (2026-08-29): defects 2+3 fixed

Defect 2: existing symbols are tokenized exactly once (the structural loop
re-tokenized both sides of every new x existing pair), a set-size-ratio
bound skips most pairwise Jaccard computations, and symbol bodies are only
extracted when duplicates are in focus. Probe commit f7180a0: analysis
7.6s -> 0.55s.

Defect 3: the bespoke fuzzy similar-name and abbreviation-table checks are
deleted (replace-and-remove). The naming focus now runs the report-side
NamingAnalyzer over the index and filters its signals (synonym splits,
ambiguous names, vague names, vocabulary outliers) to the changed symbols;
case-style checking stays (no NamingAnalyzer overlap). New issue types:
synonym_split, ambiguous_name, vague_name, vocabulary_outlier. Verified
live: a staged `to_string` reports ambiguous_name (44 sites). Known
residue: NamingAnalyzer caps its per-signal lists, so a changed name can
miss the cut on a corpus with worse offenders — bounded over-silence, and
signals only exist for names already present in the index (same
index-reflects-working-tree residue as the duplicate finder).

Defect 4's residual recalibration is subsumed: naming severities now come
from issue type (Warning for splits/ambiguous/case, Info for vague/
outliers), and risk stays the per-severity sum over scoped findings.

## Remaining defects (file scope, not fixed yet)

1. FIXED (see update above). **Not hunk-scoped — the cardinal flaw.** parse_changed_files parses whole
   changed FILES; every pre-existing symbol in a touched file becomes a
   "new symbol". Consequences: a 33-line commit reports "~100 modified
   symbols"; findings mostly describe code the change never touched;
   risk saturates at 1.00 on any commit touching a large file. Fix: build
   changed-line ranges per file from the unified diff (provider already has
   the diff) and keep only symbols whose span intersects a changed hunk.
   This single fix corrects the modified-count, definder scoping, AND risk
   saturation, and cuts analysis cost by the same factor.
2. FIXED (see 2026-08-29 update). **O(changed x whole-index) similarity, uncached.** get_existing_symbols
   walks every indexed file, copies every function body, and computes
   nesting per symbol on EVERY request (~40-60s on this repo); check_naming
   additionally deep-copies the same-type symbol list per changed symbol.
   After hunk scoping shrinks `new_symbols`, the remaining hot fix is to
   reuse content hashes / trigram prefilters before pairwise similarity,
   and stop copying SymbolInfo (const&/pointers).
3. FIXED (see 2026-08-29 update). **Naming checks are redundant and weaker than NamingAnalyzer.** The
   bespoke case-style/similar/abbreviation checks overlap the report-side
   NamingAnalyzer (synonyms, English-likeness, corpus information) and
   produce lower-precision suggestions ("consider using existing name
   make_error_response" for make_ref_sym). Candidate: back the git naming
   focus with NamingAnalyzer on the changed symbols and delete the bespoke
   path (replace-and-remove).
4. Largely addressed by 1 (inputs now scoped; probe commit shows 0.47 not 1.00). **risk_score semantics.** Currently a function of finding counts, which
   defect 1 inflates; after hunk scoping, re-calibrate or drop the single
   number in favor of the per-category counts.

## Sequencing note

Defect 1 is the gate for trusting anything git-analyze says about a
changeset; 2 falls out cheaper after 1. Recommend landing 1(+4) as one
slice with a golden on a synthetic two-hunk fixture repo before investing
in 3.
