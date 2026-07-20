# Evidence audit: all-tools replay pilot

Outcome: **fail for semantic format evidence; operational pilot complete**  
Confidence: high  
Context: authoring context

The complete 448-cell execution and transport evidence are reproducible, but the
semantic oracle gate fails. Two LLM adjudication batches were invalidated by truth
construction defects. The final grounded batch reviewed every queued case, yet six
judgments were structurally invalid and 20 correct heuristic misses could not be
promoted without broader rules accepting known-wrong cases. Selection therefore
remains blocked.

The supported claims are narrow: every live tool and both success/negative scenarios
were transported through all four encodings; 437 cells produced answers and 11
DeepSeek cells produced empty HTTP-200 completions. The data do not support a quality
ranking between JSON, XML, path records, and tagged blocks.

Required correction: define source-grounded, task-specific semantic claim schemas;
freeze positive and adversarial oracle cases for every tool; require adjudicator
completion or deterministic promotion before effect analysis; then run enough paired
repetitions for the declared threshold.
