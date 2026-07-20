# Candidate encoding exploration

These are pre-replay codec explorations, not registered experiment arms and not
evidence of model performance. A codec transforms the parsed JSON value already
serialized into an OpenCode tool-role message's `content` string. It may not change
the provider request schema, invent status/error semantics, resolve ambiguity, or
add explanatory facts.

Every candidate module exposes `NAME`, `encode(value) -> str`, and
`decode(text) -> object`. Passing the fixed conformance bank establishes mechanical
eligibility only. Provider outcomes require a new preregistration after selection.

Decoders must reject malformed, duplicate, ambiguous, truncated, and trailing input.

## Typed XML

`typed-xml-v1` represents JSON types with explicit XML elements. Object keys and
strings are base64-encoded UTF-8 with surrogate preservation; this supports XML-hostile
control characters and arbitrary JSON keys. The strict decoder rejects DTD/entity
declarations, unknown structure, duplicate keys, and non-JSON numbers. Its main risks
are XML parser surface and substantial tag/base64 overhead.

## Readable XML

`readable-xml-v1` uses the same explicit JSON type hierarchy but leaves ordinary
keys and string values as escaped XML text. It uses base64 only when XML 1.0
cannot preserve a value, including control characters, carriage returns, unsafe
attribute whitespace, and lone surrogates. This makes typical paths and source
lines visible to a model while retaining exact fallback behavior. It remains
tag-heavy: the recorded single-hit search fixture is 707 bytes versus 207 bytes
for production JSON (3.42x), before provider-level JSON string escaping.

## Semi-structured candidates

These representations are candidate generators, not registered experiment arms and
not evidence of model performance. They transform arbitrary parsed JSON values
without adding tool-specific facts or interpretations.

LCI-PATH-RECORDS/1 emits a record for every node. Paths are typed JSON arrays:
string components are object keys and integer components are array indices. Path
and payload fields are length-framed, and container records retain empty containers
and child counts.

LCI-TAGGED-BLOCKS/1 is a recursive stream with explicit scalar and container tags.
Object/array sizes delimit containers, while strings, keys, and numbers use JSON
literals with explicit lengths. Object keys are sorted in both formats so encoding
is deterministic.

Both codecs handle root scalars, null, empty containers, numeric-looking object
keys, arbitrary strings, newlines, and delimiter characters. They reject non-JSON
Python values, non-string object keys, and non-finite floats. They preserve parsed
JSON values, not insignificant source-text details such as whitespace, escape
spelling, exponent spelling, or original object-key order.
