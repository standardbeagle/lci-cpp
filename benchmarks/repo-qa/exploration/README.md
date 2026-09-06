# Exploration corpora: recovery recipe

`corpora.json` pins each corpus to a LOCAL git checkout (`source_path`) at a
`pinned_commit`. The forge (`benchmarks/repo-qa/scripts/exploration_corpus_forge.py`)
exports that commit with `git archive` and mutates the copy; it never writes
back into the source checkout.

Generated corpora land under `benchmarks/repo-qa/.work/exploration/`, which is
gitignored: corpus content is NEVER committed, only regenerated from
(source commit, forge version, seed). Losing `.work/` loses nothing that the
recipe below cannot rebuild byte-identically.

All three `source_path`s live under THIS checkout (`lci-cpp/real_projects/`).
A benchmark must never depend on a sibling repo's presence. The paths are
absolute and assume the checkout at `/home/beagle/work/core/lci-cpp`; on a
host where the checkout lives elsewhere, edit the three `source_path` values
in `corpora.json` (and the clone targets below) before forging -- the forge
fails fast naming the path if you forget.

## If a source checkout is missing

The forge fails fast and names the missing path. Rebuild the checkout, then
re-forge. The clone only needs to CONTAIN the pinned commit; the forge refuses
a HEAD that is off the pin unless you pass `--allow-head-mismatch`, so checkout
the pin explicitly.

### pocketbase

    git clone https://github.com/pocketbase/pocketbase.git \
        /home/beagle/work/core/lci-cpp/real_projects/go/pocketbase
    git -C /home/beagle/work/core/lci-cpp/real_projects/go/pocketbase \
        checkout d438c6a96a0252ff9df62c1cfe193480ed9ff15d
    /usr/bin/python3 benchmarks/repo-qa/scripts/exploration_corpus_forge.py \
        forge --corpus pocketbase --seed 7

### scikit-learn

    git clone https://github.com/scikit-learn/scikit-learn.git \
        /home/beagle/work/core/lci-cpp/real_projects/python/scikit-learn
    git -C /home/beagle/work/core/lci-cpp/real_projects/python/scikit-learn \
        checkout 0885712e436b07cd04d60e955101aaf6a8cbb709
    /usr/bin/python3 benchmarks/repo-qa/scripts/exploration_corpus_forge.py \
        forge --corpus scikit-learn --seed 7

### next.js

    git clone https://github.com/vercel/next.js.git \
        /home/beagle/work/core/lci-cpp/real_projects/typescript/next.js
    git -C /home/beagle/work/core/lci-cpp/real_projects/typescript/next.js \
        checkout 97532172c065ca2b49dd79d1fa44c3a0b864065b
    /usr/bin/python3 benchmarks/repo-qa/scripts/exploration_corpus_forge.py \
        forge --corpus next.js --seed 7

Each forge writes `<corpus>/seed-7/manifest.json` under
`benchmarks/repo-qa/.work/exploration/` and stamps `READY` only when the
corpus's configured validation passes. Determinism guarantee: same pinned
commit + forge version + seed yields a byte-identical tree (`tree_hash` in the
manifest).
