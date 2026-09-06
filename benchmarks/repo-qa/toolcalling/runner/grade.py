"""Grade one recorded trace: was the FIRST decisive tool call the right one.

This epic measures SELECTION, so eventual success does not count. A model that
flails through four tools before landing on the right one has mis-selected;
the record keeps `correct_ever` and `first_correct_position` so E2.4 can say
how far off it was, but the confusion-matrix cell is keyed on the first call.

Args plausibility is validated against the committed surface MANIFEST's
input_schema, never against the schema the mock serves: the mock currently
serves an empty permissive schema for every tool (follow-up
01M1W40XF2E0HKT05SS51A5PJ2), so grading against it would certify anything.
That is an oracle-independence requirement, not a convenience -- the checker
must not share its contract with the thing under test.

Every outcome is NAMED. A quota kill, a timeout, a provider error and a run
that called no tool at all are four different facts, and none of them is a
wrong selection; they are excluded from the matrix denominator rather than
silently scored zero.
"""

from __future__ import annotations

import json
from pathlib import Path

import jsonschema

# Statuses opencode_runner.run_opencode can report. "answered" is the only one
# that carries a gradeable trace; the rest are recorded as-is.
GRADEABLE_STATUS = "answered"


class Grader:
    def __init__(self, schemas: dict[str, dict]):
        self.schemas = schemas
        self.known_tools = set(schemas)

    @classmethod
    def from_manifest(cls, manifest: Path) -> "Grader":
        payload = json.loads(Path(manifest).read_text())
        return cls({tool["name"]: tool.get("input_schema") or {}
                    for tool in payload["tools"]})

    # ----------------------------------------------------------------- helpers

    def _validate_args(self, tool: str, args: dict) -> tuple[bool | None, list[str]]:
        schema = self.schemas.get(tool)
        if schema is None:
            return None, []
        try:
            jsonschema.validate(args, schema)
        except jsonschema.ValidationError as exc:
            return False, [exc.message]
        return True, []

    @staticmethod
    def tier(task: dict) -> str:
        return "control" if task.get("control") else "confusable"

    # ------------------------------------------------------------------ grading

    def grade(self, task: dict, calls: list[dict], run_status: str) -> dict:
        correct = task["correct_tool"]
        record = {
            "task_id": task.get("id"),
            "correct_tool": correct,
            "tier": self.tier(task),
            "run_status": run_status,
            "call_count": len(calls),
            "trace": [{"tool": c["raw_tool"], "native": c["native"],
                       "position": c["position"]} for c in calls],
            "first_called_tool": None,
            "first_call_native": None,
            "selected_correct": False,
            "correct_ever": any(not c["native"] and c["tool"] == correct
                                for c in calls),
            "first_correct_position": next(
                (c["position"] for c in calls
                 if not c["native"] and c["tool"] == correct), None),
            "args_plausible": None,
            "args_errors": [],
            "matrix_cell": None,
            "counts_in_matrix": False,
        }

        if run_status != GRADEABLE_STATUS:
            record["outcome"] = run_status
            return record
        if not calls:
            record["outcome"] = "no_tool_call"
            return record

        first = calls[0]
        if first["native"]:
            record["first_called_tool"] = f"native:{first['tool']}"
            record["outcome"] = "graded"
            record["counts_in_matrix"] = True
            record["matrix_cell"] = (correct, record["first_called_tool"])
            return record

        if first["tool"] not in self.known_tools:
            # The mock rejects an unknown name with -32602, but the SELECTION
            # already happened: the model named a tool the surface never
            # offered. That is its own outcome, not a neighbour confusion.
            record["first_called_tool"] = f"hallucinated:{first['tool']}"
            record["outcome"] = "hallucinated_tool"
            return record

        record["first_called_tool"] = first["tool"]
        record["outcome"] = "graded"
        record["counts_in_matrix"] = True
        record["matrix_cell"] = (correct, first["tool"])
        record["selected_correct"] = first["tool"] == correct
        plausible, errors = self._validate_args(first["tool"], first["args"])
        record["args_plausible"] = plausible
        record["args_errors"] = errors
        return record
