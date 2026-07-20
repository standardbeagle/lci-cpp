# Exploratory candidate selection

This is mechanical pre-screening, not a model-quality result. No provider replay
cells were run.

All four candidates passed exact canonical-value round trips on all 18 fixed cases,
including realistic LCI outputs, root scalars, empty containers, numeric-looking and
escaped keys, arbitrary delimiters/newlines, Unicode normalization distinctions, and
lone surrogates. Dedicated malformed-input tests require decoders to fail closed.

| Candidate | Bank + live bytes / JSON | Live bytes | Live ratio | Live lines | Decision |
|---|---:|---:|---:|---:|---|
| Production canonical JSON | 1.00x | 207 | 1.00x | 1 | retain control |
| Fully encoded typed XML | 4.85x | 1,120 | 5.41x | 1 | reject before replay |
| Readable XML | 3.23x | 707 | 3.42x | 1 | advance |
| Typed path records | 2.20x | 504 | 2.43x | 16 | advance |
| Tagged blocks | 1.94x | 413 | 2.00x | 27 | advance |

The fully encoded XML form is mechanically valid but dominated by readable XML for
the intended hypothesis: it is larger and hides ordinary identifiers/source text in
base64. Rejecting it does not rely on model outcomes.

The three advancing candidates are structurally complementary: hierarchical markup,
flat addressable records, and recursive line-oriented blocks. Their expansion is a
known cost, not proof against comprehension benefits. The next preregistration must
compare each against the identical production request, counterbalance order, and
define multiplicity-adjusted analysis before any replay.
