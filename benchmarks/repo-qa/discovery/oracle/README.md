# gopls ground-truth oracle

Answer keys for the pinned PocketBase corpus, produced by `scripts/gopls_oracle.py`.

The oracle's authority is gopls and nothing else. It never consults the tool
under benchmark: where gopls and that tool disagree, the disagreement is the
measurement being taken, not a discrepancy to reconcile.

## Pins

| Pin | Value |
|---|---|
| gopls | `golang.org/x/tools/gopls v0.23.0` (`PINNED_GOPLS_VERSION`) |
| corpus | PocketBase @ `d438c6a96a0252ff9df62c1cfe193480ed9ff15d` |

Either pin changing invalidates every key, by cache path and by a provenance
check inside each payload.

## Cache

    cache/<corpus-commit>/<gopls-version-slug>/<symbol-slug>__d<depth>.json

Untracked (see `.gitignore`): regenerable, and large. Cold gopls over ~150k LOC
costs seconds per symbol, so a sweep wants the cache warm.

## Usage

    scripts/gopls_oracle.py key \
        --corpus /path/to/real_projects/go/pocketbase \
        --symbol UcFirst --file tools/inflector/inflector.go --line 13 --column 6

Exits non-zero with a reason code rather than emitting a degraded key. An oracle
that quietly returns nothing grades every candidate answer as a miss, which reads
as a catastrophic tool regression that never happened.
