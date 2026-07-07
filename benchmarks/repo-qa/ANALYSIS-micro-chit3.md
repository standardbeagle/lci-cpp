# chi-t3 micro iteration — browse_file trust

Date: 2026-07-06.

Question: `chi-t3` asks the model to use `lci_browse_file` on `chain.go` and
list exported types defined in that file.

## Result

The product fix worked. `kind=type` now includes concrete type declarations
such as structs, so `browse_file {"file":"chain.go","kind":"type"}` returns
`ChainHandler` directly. The benchmark gold answer was also corrected:
`Middlewares` is a referenced field type in `chain.go`, but it is defined
elsewhere and should not be required.

The response hint improved tool-call economy for most models. In
`results/micro-chit3-browsehint-20260706`, five of six models answered without
reading the file:

| model | calls | read? |
|---|---|---:|
| godsv4pro | `lci_browse_file` | no |
| goglm52 | `lci_browse_file` | no |
| goqwen37 | `lci_browse_file`, `lci_browse_file` | no |
| nemotron | `lci_browse_file` | no |
| zhipuglm52 | `lci_browse_file` | no |
| kimik2p7 | `lci_browse_file`, `read`, `lci_search` | yes |

All six scored `facts=1.00`.

## Kimi outlier

Treat `kimik2p7` as an outlier for this micro case. The hidden/provider system
message is not exposed in opencode's stored session data. The local SQLite
store contains user, assistant, reasoning, and tool parts, not the hidden
system message sent to the provider.

The stored Kimi reasoning is enough to explain the read. After `browse_file`
returned `ChainHandler`, the hint, and the exported symbol list, Kimi reasoned
that `Middlewares` might be a type defined in `chain.go`, read the file "to be
sure," then searched for `type Middlewares`. This is a model-specific
verification habit, not a remaining `browse_file` correctness problem.

Do not use Kimi's `chi-t3` read as a general signal that `browse_file` is
insufficient. Use the mixed-model rate instead: after the fix and hint, the
read rate was `1/6`, with Kimi as the only reader.
