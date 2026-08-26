# Vendored analysis data

## scowl_english_words.txt

English word list for the vocabulary analyzer's real-word check (a token
that is real English is never a "misspelling" or "obscure-token" outlier).

- Source: SCOWL (Spell Checker Oriented Word Lists), Kevin Atkinson —
  aspell6-en-2026.02.25-0.tar.bz2 from
  https://github.com/en-wl/wordlist/releases/tag/rel-2026.02.25
  (sha256 77a5cb437c45d1115f3b593802c20651d8c93803ed1073278dc1a1240016f10d)
- Lists decoded from the aspell "precompressed" .cwl format: en-common,
  en-wo_accents-only, en_US-wo_accents-only, en-variant_0/1/2 (variant
  spellings included deliberately — this is a reality check, not a style
  linter).
- Filter: lowercased, ASCII `[a-z]{2,24}` only (possessives, hyphenations,
  accented forms, and proper-noun capitalizations dropped), deduplicated,
  sorted. 94,616 words.
- License: see SCOWL-COPYRIGHT (permissive, notice required).

The file is embedded into the binary at configure time (chunked raw string
literals — MSVC caps a single literal at 64KB) via the same mechanism as
the MCP schemas; there is no runtime file lookup. Regenerate by re-running
the decode+filter above against a newer SCOWL release.
