# Provider-request format exploration v2

V2 is a fresh exploratory study, not a continuation or retry pool for v1. It keeps the same four arms, two `opencode-go` models, balanced 64-cell schedule, oracle shape, inference family, and advancement rule.

V1 is classified only as an infrastructure pilot. Its 64 recorded attempts uniformly returned HTTP 403, so it produced no model outcomes and contributes nothing to v2 inference.

Provider execution is intentionally blocked. Before any v2 replay, the repository must contain two new successful post-tool captures, new sanitized fixtures, safe captured-header profiles, frozen digests for both artifact classes, successful authentication probes, and a new immutable analysis revision. Secret header values must never be committed or hashed. See `safe-header-contract.json` for the persisted-header boundary.

`preflight-template.json` makes every unresolved gate machine-readable and begins with `provider_execution_allowed: false`. `report-template.md` fixes the human report structure before outcomes. `report.md`, raw attempts, scoring, and analysis do not exist until all gates pass and the full grid is run.
