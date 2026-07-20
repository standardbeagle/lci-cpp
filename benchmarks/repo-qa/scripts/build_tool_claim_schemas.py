#!/usr/bin/env python3
"""Generate the curated typed claim bank while binding genuine output digests."""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from tool_claim_oracle import digest, validate_bank


SUCCESS = {
 "browse_file": ([{"id":"methods","kind":"semantic","alternatives":[["Execute","Start"]]}], "The exported methods are Execute and Start.", "Only Start is exported."),
 "code_insight": ([{"id":"go_files","kind":"integer","value":447}], "There are 447 Go source files.", "There are 448 Go files."),
 "context": ([{"id":"signature","kind":"literal","value":"func (pb *PocketBase) Start() error"},{"id":"purpose","kind":"semantic","alternatives":[["Start","starts","application"],["Start","registers","commands","executes"]]}], "func (pb *PocketBase) Start() error starts the application.", "Start is in pocketbase.go."),
 "debug_info": ([{"id":"file","kind":"literal","value":"pocketbase.go"}], "PocketBase.Start is declared in pocketbase.go.", "It is declared in app.go."),
 "find_files": ([{"id":"prod","kind":"literal","value":"forms/record_upsert.go"},{"id":"test","kind":"literal","value":"forms/record_upsert_test.go"}], "forms/record_upsert.go and forms/record_upsert_test.go.", "Only forms/record_upsert.go."),
 "get_context": ([{"id":"signature","kind":"literal","value":"func (pb *PocketBase) Start() error"},{"id":"purpose","kind":"semantic","alternatives":[["Start","starts","application"],["Start","registers","commands","executes"]]}], "func (pb *PocketBase) Start() error starts the application.", "It registers commands but the signature is unknown."),
 "git_analysis": ([{"id":"commit","kind":"sha","value":"d438c6a96a0252ff9df62c1cfe193480ed9ff15d"}], "The checked out commit is d438c6a96a0252ff9df62c1cfe193480ed9ff15d.", "The target is HEAD."),
 "index_stats": ([{"id":"ready","kind":"literal","value":"ready"}], "The index status is ready.", "The index is building."),
 "info": ([{"id":"purpose","kind":"semantic","alternatives":[["semantic code search"],["search","indexed code"],["search","indexed source"]]}], "It provides semantic code search.", "It edits indexed source."),
 "inspect_symbol": ([{"id":"definition","kind":"location","value":"pocketbase.go:166"}], "It is defined in pocketbase.go at line 166.", "It is defined in pocketbase.go at line 167."),
 "list_symbols": ([{"id":"methods","kind":"semantic","alternatives":[["Execute","Start"]]}], "The methods are Execute and Start.", "The only method is Execute."),
 "search": ([{"id":"callsite","kind":"location","value":"examples/base/main.go:119"}], "The callsite is examples/base/main.go at line 119.", "The definition is pocketbase.go:166."),
 "semantic_annotations": ([{"id":"none","kind":"absence"}], "There are no semantic annotations.", "There is an annotation at pocketbase.go:1."),
 "side_effects": ([{"id":"callee","kind":"literal","value":"Execute"}], "PocketBase.Start directly invokes Execute.", "It directly invokes Bootstrap."),
}

NEGATIVE = {
 "browse_file": ("error", "The tool returned an error because the file was not found; use find_files.", "The missing file was successfully read."),
 "code_insight": ("fallback", "It returned module summary data despite the target request.", "It returned no data."),
 "context": ("error", "It failed with an error loading the missing manifest file.", "The context manifest loaded successfully."),
 "debug_info": ("not_found", "No indexed file was found; use a project-root-relative path.", "The requested file was found."),
 "find_files": ("empty", "No files matched; try a shorter filename fragment.", "One file matched."),
 "get_context": ("not_found", "No symbol was found; use search or similar symbols.", "The symbol context was returned."),
 "git_analysis": ("error", "Git analysis failed with an error for the invalid ref.", "The range analysis succeeded."),
 "index_stats": ("fallback", "The call returned usable index summary data with ready status.", "The index request failed."),
 "info": ("corrective", "It returned available tools and corrective guidance.", "It returned details for the nonexistent tool."),
 "inspect_symbol": ("not_found", "No symbol was found; use similar symbols or search.", "The symbol definition was returned."),
 "list_symbols": ("empty", "The successful result contained zero symbols.", "It returned a method."),
 "search": ("empty", "There were zero matches; try broader flags or a different pattern.", "The requested symbol matched."),
 "semantic_annotations": ("empty", "There are no semantic annotations.", "A matching annotation was returned."),
 "side_effects": ("error", "The tool returned an error because the symbol was not found.", "Side effects were returned for the symbol."),
}


def build(matrix: dict, surface: dict) -> dict:
    cases = {(x["tool"], x["scenario"]): x for x in matrix["cases"]}
    questions = {x["name"]: x["question"] for x in surface["tools"]}
    schemas = []
    for tool in sorted(SUCCESS):
        claims, good, bad = SUCCESS[tool]
        case = cases[(tool, "success")]
        schemas.append({"task_id":f"{tool}--success","tool":tool,"scenario":"success",
                        "question":questions[tool],"source_output_digest":digest(case["output"]),
                        "required_claims":claims,"forbidden_patterns":[],"good_answers":[good],"bad_answers":[bad]})
        outcome, good, bad = NEGATIVE[tool]
        case = cases[(tool, "negative")]
        schemas.append({"task_id":f"{tool}--negative","tool":tool,"scenario":"negative",
                        "question":f"Interpret the captured negative {tool} result.",
                        "source_output_digest":digest(case["output"]),
                        "required_claims":[{"id":"outcome","kind":"outcome","value":outcome}],
                        "forbidden_patterns":[],"good_answers":[good],"bad_answers":[bad]})
    bank = {"schema":"lci.typed-claim-bank.v1","source":"independent corpus oracle plus genuine MCP output digests",
            "schemas":schemas}
    validate_bank(bank, matrix)
    return bank


def main() -> int:
    parser=argparse.ArgumentParser(); parser.add_argument("--matrix",type=Path,required=True)
    parser.add_argument("--surface",type=Path,required=True); parser.add_argument("--out",type=Path,required=True)
    parser.add_argument("--check",action="store_true"); args=parser.parse_args()
    bank=build(json.loads(args.matrix.read_text()),json.loads(args.surface.read_text()))
    text=json.dumps(bank,indent=2,sort_keys=True)+"\n"
    if args.check:
        if not args.out.exists() or args.out.read_text()!=text: raise SystemExit("claim schema bank is stale")
    else: args.out.parent.mkdir(parents=True,exist_ok=True); args.out.write_text(text)
    return 0

if __name__=="__main__": raise SystemExit(main())
