# docs/

Current, durable reference for lci-cpp:

- `TOOLS.md`: the MCP tool contract (tests read its parameter tables).
- `install-distribution.md`: the install and `lci update` contract every installer follows.
- `performance/`: suite and cold-start baselines; `scripts/` read and write the JSON files.
- `benchmarks/`: the stage-gated optimisation PR policy.
- `fleet/prompts/`: headless gate contracts the `.worktrack/templates.json` steps load.
- `reviews/`: whole-tree reviews that open worktrack tasks still cite.
- `solutions/`: compound-engineering learnings.

The website (Astro Starlight) is in `site/`. Plans, specs and in-flight work are tracked in
worktrack workspace `lci-cpp`. A plan or review a task cites is committed under `docs/plans/`
(or `docs/reviews/`) while the task is open, and removed once it lands.
