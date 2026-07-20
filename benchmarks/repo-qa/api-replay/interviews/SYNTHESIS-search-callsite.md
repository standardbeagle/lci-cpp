# Exploratory model interview: search callsite

Status: exploratory design input only; not benchmark evidence.

Frozen protocol revision: `6da2c14`

Clean sessions:

- DeepSeek: `ses_080b21b78ffe5kEsuHe3oQOAcq`
- GLM: `ses_080b16248ffeyF8F7FiUhUPOQ3`

Both models received the same captured user message, actual assistant tool call, and
production tool-result content. Neither received the expected answer, recorded final
assistant response, prior benchmark outcomes, arm labels, or the other interview.
OpenCode tools were disabled for both clean sessions.

## Result

No proposed alternative currently satisfies the frozen candidate-selection rule.
Production JSON remains the reference representation.

DeepSeek recommended a tagged payload. Its success tag, special empty tag, and error
payload add semantics that are absent from the source value. Its structured-field
proposal also changes the provider message schema rather than only tool-message
`content`. Both are out of scope for the exact one-pointer replay.

GLM recommended JSON-Pointer leaf lines (LFJ). This is promising as a separately
framed codec hypothesis, but the proposed inverse is under-specified and not generally
invertible:

- numeric object keys cannot be distinguished from array indices without container
  type records;
- a root scalar has no parent from which to infer its type or placement;
- sparse arrays and absent array elements are conflated;
- the proposed tab extension is not RFC 6901 and has no complete escape grammar;
- canonicalization recovers a JSON value, not the original JSON spelling or bytes.

GLM's schema-pinned row proposal changes error schemas and cannot preserve unknown
fields without schema negotiation. It therefore fails the generality criterion as
written.

Both models described the production content as "stringified" or "escaped" JSON.
At the provider boundary, `messages[N].content` is necessarily a JSON string, while
its string value is the ordinary serialized LCI result. Escapes visible in the outer
request are transport encoding, not an extra layer available for removal while
holding the provider request schema constant. Consequently, the suggested "bare JSON
object on wire" is not a legal treatment in this replay design.

## Next iteration

Before spending replay cells, mechanically falsify candidate codecs on a fixed
adversarial value bank containing root scalars, empty objects and arrays, numeric and
escaped keys, nested mixed containers, nulls, omitted fields, and multi-result/error
values captured verbatim from LCI. A candidate advances only with 100% canonical-value
round trips and an exact provider-request diff limited to the recorded tool content.

The next model interview should explicitly distinguish the provider's outer JSON
string field from the LCI serialization it contains, and should ask models to repair
the LFJ counterexamples rather than generate additional unconstrained formats.
