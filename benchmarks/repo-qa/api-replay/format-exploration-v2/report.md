# LCI live search format exploration v2

Status: **complete, no candidate advances; primary evidence audit failed on oracle wording coverage**

## Decision

No alternative advances to the broader benchmark. All 64 planned cells completed
successfully with HTTP 200 and no retries. GLM scored 8/8 exact for every format.
Under the frozen extractor, DeepSeek scored 7/8 for production JSON and 8/8 for each
alternative, but every paired McNemar p-value and Holm-adjusted p-value was 1.0. The
observed +0.125 candidate deltas were below the frozen +0.25 practical threshold.

The sole frozen-scored miss said `examples/base/main.go` "at line 119", which is
semantically the expected answer but is not recognized by the frozen `path:line`
extractor. A labeled post-hoc sensitivity counts it as correct; then every arm/model
is 8/8, all deltas are zero, both cycle-null checks pass, and the no-advance decision
is unchanged. The primary benchmark audit nevertheless fails its oracle gate, so the
frozen 7/8 versus 8/8 rate difference is not trustworthy evidence of a format effect.

## Results by model and arm

| Model | Arm | Frozen exact | Completion | Mean input tokens | Mean output tokens | Mean latency |
|---|---|---:|---:|---:|---:|---:|
| DeepSeek V4 Flash | JSON | 7/8 | 8/8 | 7,475 | 372.8 | 4.177 s |
| DeepSeek V4 Flash | readable XML | 8/8 | 8/8 | 7,617 | 186.2 | 2.697 s |
| DeepSeek V4 Flash | path records | 8/8 | 8/8 | 7,673 | 211.2 | 2.857 s |
| DeepSeek V4 Flash | tagged blocks | 8/8 | 8/8 | 7,602 | 375.2 | 4.247 s |
| GLM-5.2 | JSON | 8/8 | 8/8 | 7,002 | 51.6 | 2.089 s |
| GLM-5.2 | readable XML | 8/8 | 8/8 | 7,148 | 39.8 | 2.503 s |
| GLM-5.2 | path records | 8/8 | 8/8 | 7,181 | 32.9 | 6.498 s |
| GLM-5.2 | tagged blocks | 8/8 | 8/8 | 7,124 | 82.6 | 12.132 s |

Token and latency values are diagnostics only. They were not permitted to select an
arm. In particular, the large GLM latency for tagged blocks is not a quality result.

## Paired contrasts and controls

For DeepSeek, each candidate-versus-JSON frozen paired delta is +0.125 with one
candidate-only exact pair; exact two-sided McNemar p=1.0 and Holm-adjusted p=1.0.
For GLM, every delta is 0.0 with p=1.0 and adjusted p=1.0.

The exact round-trip/request-diff manipulation control passed. Authenticated probes
passed and were excluded. The frozen DeepSeek cycle-null was 1.00 versus 0.75,
delta −0.25, which fails the strict `<0.25` threshold solely because of the extractor
miss. GLM's cycle-null was 1.00 versus 1.00. Under the post-hoc semantic sensitivity,
both cycle nulls pass.

## Relationship to v1

V1 remains an infrastructure-only pilot: 64 HTTP 403 attempts, zero usable model
outcomes, and no data pooled into v2. V2 used new captures, explicit safe-header
profiles, successful pre-grid probes, a new frozen revision, and a separate result
directory.

## Claim boundary

This single-callsite study does not establish a general JSON, XML, or semi-structured
format advantage and cannot justify production rollout. Its robust decision is only
that none of the three alternatives met the preregistered advancement rule. The next
iteration should repair and refreeze the oracle with equivalent `path:line` and
`path at line N` discrimination cases before testing more tools.
