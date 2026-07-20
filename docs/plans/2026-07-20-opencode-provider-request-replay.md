# OpenCode provider-request replay benchmark

## Decision and boundary

This experiment decides whether specific fact-equivalent LCI output renderings
deserve production implementation work. It measures the model continuation
from a genuine OpenCode interaction after a tool call. It does not measure
tool selection, live LCI correctness, repository navigation, or task success.

The experimental boundary is the provider request produced by OpenCode. The
OpenCode session API is not the treatment boundary because it does not accept
arbitrary assistant tool-call and tool-result parts. A local forwarding proxy
will capture the OpenAI-compatible provider request after OpenCode has composed
its system prompt, user message, assistant tool call, and tool-role result.

## Capture contract

For each LCI tool, record one genuine OpenCode trajectory selected by a frozen
task rule. Store a sanitized request body, OpenCode version, provider protocol,
model ID, tool definition digest, tool arguments, raw LCI result digest,
corpus revision, and capture time. Store only an allowlist of non-secret
headers. Never persist authorization, cookies, account identifiers, or proxy
credentials.

The proxy forwards the request and response without mutation. Protocol
fixtures exercise this contract hermetically but are not empirical captures
and cannot authorize a provider run.

## Paired replay

Clone the captured request for two opaque arms. `trace_17` retains the recorded
production content. `trace_42` replaces only the selected tool-role `content`
with a deterministic renderer. The renderer must be invertible to canonical
JSON equal to the source output. A recursive structural diff must find exactly
one changed JSON pointer. The null control uses the identity renderer and must
produce byte-identical provider bodies.

Send paired bodies directly to the captured provider endpoint. Preserve the
model, system prompt, messages, tool call ID, options, and stream setting.
Counterbalance arm order by task, model, and repetition. Do not reuse a model
conversation or expose one arm's answer to the other.

## Bank and execution gates

The confirmatory bank requires all 14 live LCI tools. Each task's independent
oracle must require only facts machine-proven to exist in both arm outputs.
Tasks with added facts, resolved ambiguity, or non-invertible renderings are
excluded before execution or explicitly registered as idealized capability
work in a different experiment.

Do not send provider requests until:

1. Sanitized live captures replace the hermetic protocol fixtures.
2. Every request pair passes the exact-one-pointer or null-equivalence gate.
3. Oracle good, wrong, omission, and invention cases pass.
4. The manifest digests and analysis revision are updated and committed.
5. Focused and full repo-QA tests pass.

The frozen manifest defines models, repetitions, failure handling, metrics,
noise, practical threshold, stratification, and reporting.
