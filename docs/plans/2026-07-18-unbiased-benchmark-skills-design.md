# Unbiased benchmark skills design

## Goal

Create repository-local guidance that prevents leading evaluations and turns benchmark conclusions into auditable evidence. The design separates experiment construction from evidence auditing while sharing the same validity principles.

## Packages

`design-unbiased-benchmarks` governs work before and during data collection. It requires a decision statement, narrow claim boundary, preregistration, equivalent arms, neutral prompts, independent discriminating oracles, controls, repeated runs, frozen analysis, explicit failure handling, and complete reporting. A deterministic script validates manifest structure without claiming to validate scientific judgment.

`audit-benchmark-evidence` governs review after artifacts exist. It reconstructs temporal order, compares arms fact by fact, checks prompts and leakage, recomputes the grid and analysis, tests controls, and maps each claim to direct evidence. Hard validity gates prevent a numerical average from masking a fatal defect. A deterministic script validates structured gate records and derives a fail-closed outcome.

## Independence model

Independence comes from procedure and artifacts: withheld conclusions, raw inputs, immutable revisions, recomputation, and explicit evidence. Fresh contexts or subagents can reduce context contamination, but they remain context-management tools. Their agreement or identity carries no evidentiary weight.

## Outcomes

The audit returns `pass`, `pass_with_changes`, `fail`, or `inconclusive`. A failed hard gate forces failure. Missing required evidence produces an inconclusive outcome. Only bounded defects that cannot plausibly change headline conclusions permit `pass_with_changes`.

## Validation

Validate both skill packages with the standard skill validator. Self-test both deterministic scripts. Forward-test the design skill on an intentionally leading A/B request and the audit skill on raw benchmark artifacts without supplying the expected diagnosis.
