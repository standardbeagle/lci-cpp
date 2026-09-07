# Recorded provider streams

Real `opencode run --format json` stdout, captured so the hermetic tests are
shaped by the provider's actual event stream rather than by hand-written data
authored from the same mental model as the parser.

| File | Captured | Model | Task / arm |
|---|---|---|---|
| `opencode-go-deepseek-v4-flash.file-lines.shape_17.jsonl` | 2026-09-06 | `opencode-go/deepseek-v4-flash` | `file-lines` / `shape_17` |
| `opencode-go-deepseek-v4-flash.file-lines.shape_42.jsonl` | 2026-09-06 | `opencode-go/deepseek-v4-flash` | `file-lines` / `shape_42` |

Capture conditions: the tool-denied, corpus-free git workspace built by
`opencode_runner.empty_git_workspace`, prompt from `response_shape_ab.PROMPT`
at the committed manifest/task digests. Both cells returned `answered`.

These streams carry shapes no fixture author reliably invents: hyphenated part
types (`step-start`, `step-finish`), a `tokens` block nesting a `cache`
sub-object, and text split across a message id. Re-capture only alongside a
manifest revision, and record the new date here.
