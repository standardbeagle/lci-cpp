# Scope-based type resolution (SCIP base case) — call-graph precision

## Problem
`resolve_reference_target` is name-string based (same-file → import/package
disambiguation → first candidate). No receiver-type resolution, so `x.M()` on a
common method name (ServeHTTP/String/Error/Close/Get…) can attribute the call to
the wrong same-named symbol. Qualified function calls (`pkg.Func()`) resolve
well; method calls on receivers do not.

## Approach (no type checker, no generics instantiation, no flow analysis)
Resolve the receiver's type from a **per-function local type environment** built
purely syntactically in the extractor, then emit method-call refs as
**receiver-type-qualified** names `Type.M`; the resolver matches `Type.M` to the
method symbol whose receiver type is `Type`. Unknown receiver type → bare `M`
(today's name-based path). Interface/dynamic dispatch → candidate set (honest;
same as gopls/SCIP).

## Components
1. **Local type env** (extractor, per function): `{name → type}` from
   receiver, typed params, and simple decls. Cleared on function entry.
2. **Qualified emission**: `recv.M` where `typeof(recv)` known → ref
   `referenced_name = "Type.M"`.
3. **Resolver**: dotted `Type.M` → candidates named `M` filtered by receiver
   type (parsed from each candidate's signature) → exact; else fall back.

## Per-language base case (env population rules)
| Lang | receiver/self | local decls that yield a type |
|---|---|---|
| Go | `(r *T)` | `x := T{}`, `x := &T{}`, `var x T`, typed params, `x := NewT()`(ret type) |
| Java | `this`→class | `T x`, `new T()`, typed params |
| C# | `this`→class | `T x`, `var x = new T()`, typed params |
| TypeScript | `this`→class | `const x: T`, `x = new T()`, `(x: T)` |
| Python | `self`→class (scope) | `x = T(...)`, `x: T`, `def m(self, x: T)` |
| Rust | `&self`→impl type | `let x: T`, `let x = T::new()`, typed params |
| C++ | `this`→class | `T x;`, `T* x = new T()` |
| JS | `this`→class | `x = new T()` |
| Kotlin | `this`→class | `val x: T`, `x = T()` |
| PHP | `$this`→class | `$x = new T()` |
| Ruby | `self`→class | `x = T.new` |
| Zig | — | `var x: T`, `T{}` |

## LCI guideline fit
- Write-path only (extract/link); reads stay lock-free RCU. 
- Env built once per function; resolution = hash lookups + cached.
- Deterministic; candidate sets sorted.
- Base case only; name-based fallback; per-language rules isolated like
  `process_<lang>_reference`.
- Honest: unknown/dynamic → candidate set, never a fabricated single edge.

## Rollout (each phase: implement → measure precision on a real corpus → goldens)
1. Go (reference impl; chi/pocketbase) — proves architecture.
2. Java / C# / TypeScript (explicit types — cheapest).
3. Python / Rust (annotations + constructor inference; fastapi + a rust repo).
4. JS / C++ / Kotlin / PHP / Ruby / Zig.

## Status
- [ ] Phase 1 Go
- [ ] Phase 2 Java/C#/TS
- [ ] Phase 3 Python/Rust
- [ ] Phase 4 remainder
