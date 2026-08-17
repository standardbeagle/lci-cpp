# AST Retention vs Compact IR (post-C++ move)

Status: research/design (2026-08-17). Question: now that we own the C++
tree, should we retain ASTs — or convert them into a compact form — to
unlock more powerful analysis (post-hoc AST queries, CFG/dataflow-class
checks) than the current parse-and-discard model allows?

## Ground truth (surveyed, cited)

- Every `UniqueTree` dies at scope end; no tree is stored anywhere
  (`pipeline_processor.cpp:61`, `mcp.cpp:64`, `git/analyzer.cpp:183`,
  `linker_engine.cpp:87`). No TSTree memory measurement exists in-repo.
- Content is already mmap-retained (`5e9f5f5`): `FileService::get_content`
  returns a zero-copy view; content-store heap 102.8→2.5MB, server RssAnon
  0.62x corpus. **Re-parse I/O is free for indexed files.** Parser pool is
  grammar-warmed. Skip gates (`max_parse_file_size` 2MB, trigram-hostile)
  bound the worst case.
- The RSS self-cap measures **RssAnon** (`server.cpp:88-107`) exactly so
  file-backed mmaps don't count. Retained trees are anon heap — they count.
  Budget target ≤2x corpus, fought for byte-by-byte (interned scope chains,
  32B StoredRef, 64B token ceiling).
- The side-effect pass consumes body facts *during* the walk via a sink
  (`set_side_effect_sink`); there is no post-hoc query API. git analysis
  parses **blob content at a ref** — never retainable by definition.

## Options

### A. Retain full TSTrees — rejected
tree-sitter trees typically run ~10-20x source bytes (unmeasured here —
see prerequisites), all anon heap. A 100MB corpus would add O(GB) against
a cap the whole memory epic exists to defend. Even LRU-bounded, it buys
only "query the syntax again", not better analysis primitives.

### B. Compact semantic-op IR (recommended durable layer)
Extract-once, per-function packed **op stream** — the interned-scope-chain /
StoredRef discipline applied to bodies. One entry per analysis-relevant
event, in source order:

    op:u8 (call, assign, branch-enter/exit, loop-enter/exit, throw,
           catch-enter(kind), finally-enter, defer-register, acquire,
           release, return(kind), err-check, block-enter/exit)
    line-delta: varint
    operand: optional interned u32 (callee name id, resource class id)

~2-6B per op; bodies average tens of ops → ~100-300B/function, single-digit
MB on large corpora — priced like postings, censused by `memprofile`
(add a section; `SideEffectInfo` today is the un-budgeted string-heavy
proto-IR and needs this packing regardless).

What it unlocks: micro-CFG per function (block nesting is in the stream),
dataflow-lite (err assigned → checked before overwrite/return; acquire →
release on all paths), swallow/leak detection re-runnable **post-index
without re-parse**, cross-language uniform analysis surface, and future
scores iterate on stored IR instead of re-walking trees. The error-handling
epic's detectors become IR consumers instead of one-shot walk hooks.

### C. AST-on-demand (escape hatch, cheap to add)
For queries the IR didn't anticipate: re-parse the one file on demand —
mmap view + pooled parser + `ts_parser_parse_string`. Expected ~ms/file
(measure first). Optional tiny LRU of trees for repeated queries on the
same file; likely unnecessary. This turns "post-hoc AST queries" from a
memory question into a latency question, and only for the files a query
actually touches.

### D. Status quo multi-sink
Keep parse-and-discard but attach multiple sinks to one walk. Cheapest,
but every new analysis stays extract-time-only and warmup-coupled; rejected
as the end state, though the sink API remains how the IR gets *built*.

## Recommendation

**B + C.** The op-stream IR is the durable, budgeted layer that makes
analyses (error handling, resource management, future dataflow checks)
post-hoc and re-runnable; on-demand re-parse covers the long tail. No full
tree retention.

## Prerequisite measurements (before implementation)

1. TSTree resident bytes/source byte and parse-vs-extract wall split —
   `mallinfo2` delta + sub-timers at the existing `result.stage`
   transitions (`pipeline_processor.cpp:160-256`). `ProcessedFile::duration`
   exists but is single-bucket and unread.
2. `SideEffectInfo` corpus-wide footprint (memprofile census section).
3. Re-run soak ceilings post-`5e9f5f5` (current 6.4GB/12.8x dotnet figure
   predates mmap retention).

## Incidental defects found by the survey (fix independent of this design)

1. **Per-file reindex loses symbols**: `MasterIndex::index_file`/
   `update_file` (`master_index.cpp:336-396`) update content/trigram/
   postings but never run extraction — a per-file reindex silently drops
   that file's symbols/refs/scopes. Symbols only populate on the bulk
   pipeline. Silent-fallback class defect.
2. **FileWatcher/DebouncedRebuilder are unwired** — no production
   instantiation despite `watch_mode=true` default; the freshness argument
   in `file_content_store.h:29-34` rests on a watcher that isn't running.
3. MCP warmup re-reads files via `ifstream` heap copy (`mcp.cpp:56-61`)
   instead of `FileService::get_content` mmap views, serially, off the
   worker pool.
4. Complexity matched to symbols by O(symbols × points) linear scan per
   file, duplicated (`pipeline_processor.cpp:76-82`, `git/analyzer.cpp:209`).

## Incremental parsing (side note)

`old_tree` is nullptr at every parse site; no `ts_tree_edit` anywhere.
Incremental parse only pays with a retained previous tree — under this
design that exists only inside the optional LRU (C), so treat it as a
watcher-path optimization after defects 1-2 are fixed, not a goal.
