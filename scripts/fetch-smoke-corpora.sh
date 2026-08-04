#!/usr/bin/env bash
# Fetches the large long-tail corpora used by index_size_smoke_test.cpp.
#
# Each repo is shallow-cloned once into .work/smoke-corpora/<name> and the
# checked-out SHA is recorded in .work/smoke-corpora/manifest.txt so a run
# is reproducible after the fact. Re-running skips repos already present.
# These are deliberately BIG repos across languages — the smoke test's job
# is to prove indexing memory stays bounded on exactly the corpus class
# that OOM'd hosts (2026-08-04 incident: 26 GB RSS on the mongodb driver).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${LCI_SMOKE_CORPORA_DIR:-$ROOT/.work/smoke-corpora}"
MANIFEST="$DEST/manifest.txt"
mkdir -p "$DEST"

# name  url                                                language
CORPORA="
nextjs      https://github.com/vercel/next.js.git          typescript
dotnet      https://github.com/dotnet/runtime.git          csharp
rails       https://github.com/rails/rails.git             ruby
symfony     https://github.com/symfony/symfony.git         php
sklearn     https://github.com/scikit-learn/scikit-learn.git python
kubernetes  https://github.com/kubernetes/kubernetes.git   go
cargo       https://github.com/rust-lang/cargo.git         rust
spring      https://github.com/spring-projects/spring-framework.git java
"

echo "$CORPORA" | while read -r name url lang; do
  [ -z "$name" ] && continue
  dir="$DEST/$name"
  if [ -d "$dir/.git" ]; then
    echo "skip $name (present)"
    continue
  fi
  echo "clone $name ($lang)"
  git clone --depth 1 --quiet "$url" "$dir"
  sha=$(git -C "$dir" rev-parse HEAD)
  size=$(du -sm "$dir" | cut -f1)
  echo "$name $sha ${size}MB $lang $(date -u +%F)" >> "$MANIFEST"
done

echo "corpora ready under $DEST"
cat "$MANIFEST" 2>/dev/null || true
