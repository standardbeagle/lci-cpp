# Enhanced names: attribute-set or computed display names for low-information symbols

Status: design only (2026-08-27). Pairs with the corpus-information naming
signal (`NameInformation` in naming_analyzer) that identifies which names need
enhancement at all.

## Problem

The information score finds names that don't identify anything: `process`
(vague — tokens match everything), `to_string` ×43 (ambiguous — exact
collisions), `iprefix` (obscure — unguessable). Reports and get_context
currently render these symbols by their raw name + location. An agent reading
`process (pipeline.cpp:88)` still has to open the file to learn what it is.
An **enhanced name** is a display-layer identity that carries the missing
information without renaming the code.

## Three sources, in trust order

1. **Author-set attribute (highest trust).** Extend the existing `@lci:`
   comment-annotation machinery (SemanticAnnotator already parses `@lci:`
   labels; `@lci:entry` precedent) with:
   - `@lci:name <display-name>` — a better handle, e.g.
     `@lci:name drain-worktrack-queue` above a function named `run`.
   - `@lci:desc <text>` — one-line description surfaced beside the name.
   Storage: same annotation store the propagator reads; zero new infra.
   These are ubiquitous-language declarations — the author's own vocabulary,
   never guessed.

2. **Computed qualification (default, no author input).** For a symbol whose
   name scores below the information bar, compute the cheapest token set that
   makes it unique, in priority order:
   - parent scope (`Config::process` beats `process`),
   - file basename token(s) not already in the name
     (`process [pipeline_scanner]`),
   - highest-information co-occurring token from its call-graph community
     (the Louvain clusters already computed in insight_graph) —
     `process [cluster: reindex/watch]`.
   Deterministic, index-derived, and honest: rendered in brackets so it can
   never be mistaken for the real identifier (searching it must not mislead).

3. **LLM-written gloss (offline, cached).** For exported low-information
   symbols, an offline loop (not request-path) asks a model for a one-line
   description from the symbol's get_context packet, stores it as a
   `@lci:desc`-equivalent annotation in the index sidecar, and marks it
   `source=llm` so it is never confused with author intent. Wenyan variant:
   the gloss can be stored in wenyan (ultra-compressed classical register,
   ~2-4 hanzi per concept) for token economy — e.g. `process` →
   `批檔更新索引` (~6 tokens where the English sentence is ~15). Rendering
   picks register per consumer: wenyan into agent-facing LCF payloads (the
   report's reader is a model; hanzi density is a feature), English into
   human-facing output. Requires a `display_language` knob in .lci.kdl.

## Where enhanced names surface

- `== VOCABULARY ==` vague/ambiguous/obscure lines: append the computed
  qualification so every flagged name arrives with its fix.
- `get_context` / `inspect-symbol`: show `name`, `enhanced_name`, `desc`
  (with `source: author|computed|llm`).
- `search` results: unchanged (identifiers must stay byte-exact for edit
  round-trips); enhancement is display metadata only.

## Non-goals

- Never rewrite identifiers in code output or suggest renames automatically
  (that's the existing outlier `suggested` field's job, misspellings only).
- No request-path LLM calls (karpathy: no perf regressions on hot paths).
- Tier 3 blocked on nothing, but ships only with the cache + provenance
  fields; a gloss without `source` labeling is a fabrication risk.

## Sequencing

1. Computed qualification in the VOCABULARY emitter (pure index data).
2. `@lci:name` / `@lci:desc` parsing + get_context surfacing.
3. Offline gloss loop + wenyan register knob.
