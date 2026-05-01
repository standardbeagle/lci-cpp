# Runtime, Storage, and Allocation Redesign

## Problem

`lci-cpp` has two classes of problems.

First, server lifecycle is not reliable. `/shutdown` can report success while the
server process remains alive. Restart logic can then unlink the socket and start
another server before the old one exits. That leaves orphaned processes and
confusing per-root behavior.

Second, cold indexing is too expensive. The current path double-loads files,
clones whole file-content snapshots during incremental loads, and serializes too
much publish work. Warm queries are already fast. The main problem is the cost
of building the live index.

The redesign must fix correctness first, then remove algorithmic and allocation
waste. It must also move the core toward file-scoped ownership and simple
reclamation, with low synchronization overhead.

## Requirements

1. Use a strict two-mode runtime for each root: **Indexing** or **Searching**.
2. Block search-like work while indexing is active.
3. Keep a small set of non-search endpoints available during indexing:
   readiness, status, stats, and targeted debug surfaces.
4. Make shutdown deterministic. A successful shutdown must leave no live server
   for that root.
5. Prefer simple, bounded algorithms: hashing, trigram filtering, postings
   filtering, and candidate reduction before expensive comparisons.
6. Be strict about memory allocation. File-derived data should move toward
   file-scoped arenas and root-scoped pooled allocators.
7. Keep the indexing pipeline map-reduce shaped: push almost all work into the
   parallel map phase, and keep reduction narrow and contention-light.

## Recommended approach

Use a staged redesign.

Do not begin with a full allocator rewrite. First make runtime ownership and
mode transitions explicit. Then remove the worst indexing pathologies. Once the
ownership model is clean, introduce file arenas and low-sync allocators behind
that stable boundary.

This is the right order because the current problems are not just “too many
allocations.” They are also semantic: shutdown is not authoritative, mode
transitions are weak, and storage publication is too expensive. A full
allocator-first rewrite would increase risk before those boundaries are fixed.

## Alternatives considered

### 1. Staged storage-core rewrite

This is the recommended plan. It fixes lifecycle semantics, removes duplicate
work, adds bounded fast-path filters, and then introduces file-scoped arenas.
It gives the largest correctness gain early and creates a clean foundation for
custom allocators.

### 2. Full allocator-first rewrite

This would push arenas and custom allocators into file storage, symbols,
references, and indexes immediately. It matches the desired end-state but is
too risky as a first move because it couples shutdown, indexing, publishing,
search, and memory ownership in one large change.

### 3. Hot-path patching only

This would fix shutdown and the worst cold-start regressions while leaving the
existing ownership model largely intact. It is lower risk, but it does not meet
the longer-term requirement for strict allocation discipline and simple unload
semantics.

## Target runtime architecture

Each indexed root should own a `RootRuntime`. It is the only authority for that
root’s server lifecycle, mode transitions, file table, published index
generation, and allocator domains.

`RootRuntime` should own:

- the listener and request gate
- runtime mode and generation state
- indexing coordinator
- published file table
- published symbol/reference/search indexes
- root-scoped allocators and scratch pools

Each indexed file should become a `FileRecord`. A file record owns:

- stable `FileID`
- file path and fast content hash
- optional stronger content hash
- raw content or mapped content handle
- parsed symbol/reference/scope payloads
- file-local allocator or arena

The publication rule is simple: build a replacement file record off-thread,
publish it atomically into the active generation, then retire the old file
arena wholesale. That gives simple unload behavior and avoids per-object free
work.

Indexing should use a map-reduce shape.

- **Map phase:** scan, load, hash, parse, extract symbols and references, build
  file-local trigram/postings payloads, and allocate all file-derived objects
  into file-local arenas.
- **Reduce phase:** a narrow publish step merges or swaps file-local results into
  root-level published indexes with as little shared contention as possible.

The rule is to move as much work as possible into the parallel map side. The
reduce side should publish compact prepared payloads, not redo parsing or large
amounts of allocation-heavy work.

## Operational contract

Each root has these states:

- `Starting`
- `Indexing`
- `Searching`
- `ShuttingDown`
- `Stopped`

The normal runtime contract has two serving modes: `Indexing` and `Searching`.
Search-like operations are not served during indexing. Instead, they wait on the
active indexing generation and resume only after the runtime publishes a new
searchable generation.

A small set of non-search endpoints remains available during indexing:

- readiness/status
- stats
- narrow debug surfaces that do not depend on the live searchable index

This keeps the Go-style simplification: no mixed “maybe ready” search behavior,
no search over partially published structures, and no ambiguity about which
generation a request sees.

Shutdown overrides both modes. Once the root enters `ShuttingDown`, it rejects
new work, cancels queued waiters, drains allowed in-flight operations, stops the
listener, retires runtime resources, removes the socket and pidfile, and exits.

## Phase 1: lifecycle and ownership cleanup

Fix the runtime contract before allocator work.

1. Make `/shutdown` authoritative. It must stop the listener and drive the root
   to `Stopped`, not only set an internal flag.
2. Change `run_server()` to wait on runtime state, not on incidental thread
   behavior.
3. Add a per-root pidfile or equivalent process identity record.
4. Change restart logic so it never unlinks the socket or spawns a replacement
   server until the prior process is confirmed dead.
5. Replace blind sleeps with polling on terminal state, pid exit, and socket
   removal.

This phase removes orphaned servers and gives one root exactly one live server.

## Phase 2: cold-indexing path cleanup

This phase removes obvious waste without changing user-facing behavior.

1. Eliminate the double file-load path. The current pipeline loads files in the
   producer and then loads them again in worker threads.
2. Replace per-file snapshot cloning with batched publish for full indexing.
3. Reshape indexing around a strict map-reduce split. The map side should do all
   file-local parsing, hashing, trigram bucketing, postings extraction, and
   file-arena construction before reduce begins.
4. Keep deterministic file ordering, but do not buffer or sort more than needed
   to preserve deterministic publication.
5. Use a narrow reduce step. Prefer one or a few reducer goroutine equivalents
   that merge prepared file-local payloads with minimal shared-state contention.
6. Enable existing parallel merge machinery only where it preserves deterministic
   visibility and does not add memory churn.

The goal is to keep the current external contract while removing the largest
avoidable costs from cold startup.

## Phase 3: file arenas and low-sync allocators

After ownership is clear, shift file-derived data into file arenas.

Each file parse should allocate symbols, references, scopes, and related parsed
metadata from a file-scoped arena. Replacing or unloading a file then becomes:

1. build a new file record and arena
2. publish the new record
3. retire the old arena in one step

Use root-scoped slab or pool allocators for shared long-lived structures such as
posting entries, trigram locations, and symbol-store payloads where stable
identity matters across file generations. Use scoped scratch arenas for
per-query temporary work so searches and analyses do not pound the global heap.

Default policy:

- file-derived objects: file arena
- generation-scoped publish artifacts: generation arena or batched pool
- short-lived query temporaries: scratch arena
- tiny shared hot objects: slab/pool allocator

The default should be allocator-aware core code, not opportunistic heap use.

## Algorithmic rules

The core system should prefer bounded, low-allocation filters before expensive
work.

1. Use fast hashes for equality and cache validation.
2. Use trigram and postings filtering as the primary candidate reducers.
3. Use normalized-hash buckets before structural duplicate comparison.
4. Bound comparison sets before fuzzy or structural matching.
5. Consider Bloom filters only when they remove a measured hot miss path at low
   memory cost. Do not add them by default where trigram plus hashing already
   provides a strong filter.

The guiding rule is simple: no structure stays unless it wins on both CPU and
memory in a real workload.

## `git-analyze` implications

`git-analyze` is a secondary optimization target. Warm `git-analyze` on an
already indexed root is fast. The larger issue is the cost of reaching a
searchable root.

After the indexing path is fixed, tighten `git-analyze` itself:

1. Avoid whole-index scans where a filtered candidate set will do.
2. Cache or publish symbol-body metadata that is currently reconstructed.
3. Use content hashes and normalized fingerprints before structural comparison.
4. Keep duplicate and naming analysis bounded by candidate filters and
   configured finding limits.

Do not optimize `git-analyze` first while cold indexing remains dominant.

## Error handling and synchronization rules

1. Root state is authoritative.
2. Search handlers must not inspect partially published internals.
3. Publish happens by generation swap, not piecemeal mutation of the active
   read view.
4. Shutdown must be explicit, observable, and terminal.
5. Allocator ownership must follow object lifetime boundaries. Cross-boundary
   ownership should be represented by IDs or stable handles, not raw object
   borrowing.

Synchronization should be minimal and deliberate:

- lock-free or read-mostly access for published generations
- single-writer or narrow reducer model for publication
- file-local allocation to reduce global allocator pressure
- no global heap churn in hot search paths without a measured reason

## Validation

Validation must cover correctness, performance, and memory discipline.

### Lifecycle

- one live server per root
- `/shutdown` leaves no live process for that root
- stale-server restart never races the old process
- socket removal happens only after process exit

### Performance

- cold index time on `lci` and `lci-cpp`
- files/sec and symbols/sec
- peak RSS
- publication latency
- warm search and warm analysis latency

### Allocation and reclaim

- replacing a file retires the whole old file arena
- unloading a root reclaims root-owned arenas and pools
- search-like requests during indexing do not allocate unbounded waiter state

### Algorithmic regression bars

- candidate filtering remains bounded
- duplicate detection does not fall into whole-repo quadratic behavior without a
  prefilter
- `git-analyze` remains capped by candidate reduction and finding limits

## Success criteria

The redesign is successful when these statements are true:

1. Shutdown is deterministic and leaves no orphaned server for a root.
2. The runtime exposes a strict `Indexing` vs. `Searching` contract for
   search-like operations.
3. A narrow set of non-search endpoints remains available during indexing.
4. Cold indexing removes the worst avoidable storage and publish overhead.
5. Warm queries remain effectively instant.
6. File replacement and unload become cheap because ownership is file-scoped.
7. Core code defaults to low-sync, allocator-aware behavior instead of general
   heap churn.

## Immediate implementation order

1. Fix shutdown state transitions and per-root process identity.
2. Remove duplicate file loading and snapshot-copy-heavy full indexing.
3. Introduce a batched publish path for indexing generations.
4. Add the strict request gate for `Indexing` vs. `Searching`.
5. Move parsed file payloads into file arenas.
6. Add allocator-aware scratch paths and tighten algorithmic filters.

That order fixes the real correctness bug first, then attacks the dominant cold
path, then moves the system toward the allocator and lifetime model it should
have by default.
