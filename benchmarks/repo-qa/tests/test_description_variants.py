"""Gates for the E2.2 A/B/C/D tool-description variant sets.

The E2.3 experiment swaps ONE thing -- the description text the model sees --
and measures the shift in the selection confusion matrix. Everything else must
be provably constant, and every claim a variant makes must be true of the live
tool, or the experiment measures wording plus an invented capability and cannot
attribute the delta.

Four arms, one file each under toolcalling/descriptions/:

  A  the live description VERBATIM (byte-equal to the committed surface
     manifest, and to a live tools/list when the binary is present).
  B  task-framed  -- "Use when you need to ..."
  C  example-augmented -- a worked invocation whose keys are REAL parameters of
     that tool's live input schema.
  D  disambiguated -- one clause contrasting the tool's ACTUAL confusable
     neighbor from the E2.1 map in toolcalling/tasks/*.json.

Adversarial gates (.claude/rules/bench-harness-oracle-independence.md): each
check is paired with a discrimination case that injects the exact defect the
check exists to catch -- a variant file that carries schema material, a D
variant naming a non-neighbor, an example citing a parameter the tool does not
have, a reworded A.

Hermetic: the mock MCP only. The single live probe is the A-verbatim freshness
check, which SKIPS with a named reason when the binary or corpus is absent --
absence is never treated as agreement.
"""

from __future__ import annotations

import importlib.util
import json
import os
import re
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
REPO_QA = ROOT / "benchmarks/repo-qa"
MANIFEST = REPO_QA / "comprehension/surface/tool-surface.json"
TASK_DIR = REPO_QA / "toolcalling/tasks"
VARIANT_DIR = REPO_QA / "toolcalling/descriptions"
BASELINE_DESCRIPTIONS = REPO_QA / "toolcalling/mock_mcp/descriptions-baseline.json"
MOCK_SCRIPT = REPO_QA / "scripts/mock_lci_mcp.py"
LCI_BIN = ROOT / "build/release/src/lci"
LIVE_CORPUS = REPO_QA / ".work/pocketbase-base"

sys.path.insert(0, str(REPO_QA / "scripts"))

VARIANT_FILES = {
    "A": VARIANT_DIR / "variant-a-live-verbatim.json",
    "B": VARIANT_DIR / "variant-b-task-framed.json",
    "C": VARIANT_DIR / "variant-c-example-augmented.json",
    "D": VARIANT_DIR / "variant-d-disambiguated.json",
}
AUTHORED = ("B", "C", "D")

ALLOWED_KEYS = {"schema", "version", "variant", "note", "descriptions", "na"}
D_ONLY_KEYS = {"neighbor_contrast"}


def _load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    assert spec.loader
    spec.loader.exec_module(mod)
    return mod


mock_mcp = _load("mock_lci_mcp", MOCK_SCRIPT)


def live_tools() -> dict[str, dict]:
    return {t["name"]: t for t in json.loads(MANIFEST.read_text())["tools"]}


def live_descriptions() -> dict[str, str]:
    return {name: t["description"] for name, t in live_tools().items()}


def schema_properties(tool: dict) -> set[str]:
    return set((tool["input_schema"].get("properties") or {}).keys())


def neighbor_map() -> dict[str, set[str]]:
    """The E2.1 confusable-neighbor map, DERIVED from the task set.

    Re-typing it here would let D's contrast clauses drift from the map the
    confusion matrix is actually scored against.
    """
    out: dict[str, set[str]] = {}
    for path in sorted(TASK_DIR.glob("*.json")):
        task = json.loads(path.read_text())
        out.setdefault(task["correct_tool"], set()).update(
            task.get("confusable_neighbors", []))
    return out


def load_variant(key: str) -> dict:
    return json.loads(VARIANT_FILES[key].read_text())


# --------------------------------------------------------------- the validator

# A parameter or tool name is written in backticks; nothing else may be. The
# scanner is deliberately independent of how the text was AUTHORED -- it reads
# the emitted string, not an author-supplied list of what the string cites.
_BACKTICKED = re.compile(r"`([^`]+)`")
# A worked invocation is a JSON object literal in the description text.
_JSON_OBJECT = re.compile(r"\{[^{}]*\}")


def validate_variant(payload: dict, tools: dict[str, dict],
                     neighbors: dict[str, set[str]]) -> list[str]:
    """Return every reason this variant set is not a legal description swap.

    Legal means: it changes description text and NOTHING else -- no tool names
    added or dropped, no schema material, no claim about a parameter the tool
    does not have, no contrast against a tool that is not a real neighbor.
    """
    problems: list[str] = []
    variant = payload.get("variant")
    allowed = set(ALLOWED_KEYS)
    if variant == "D":
        allowed |= D_ONLY_KEYS
    for key in sorted(set(payload) - allowed):
        problems.append(f"unexpected top-level key {key!r}: a variant may only "
                        f"change description text")
    if payload.get("schema") != "toolcalling_descriptions_v1":
        problems.append("wrong schema tag")
    descriptions = payload.get("descriptions") or {}
    na = payload.get("na") or {}
    for name, text in sorted(descriptions.items()):
        if not isinstance(text, str):
            problems.append(f"{name}: description must be text, not "
                            f"{type(text).__name__} (schema material is not a "
                            f"description swap)")
            continue
        if name not in tools:
            problems.append(f"{name}: not a live tool")
            continue
        if not text.strip():
            problems.append(f"{name}: empty description")
        props = schema_properties(tools[name])
        for cited in _BACKTICKED.findall(text):
            if cited in tools:
                continue
            if cited not in props:
                problems.append(
                    f"{name}: cites `{cited}`, which is neither a parameter of "
                    f"its live input schema nor a live tool")
    for name, reason in sorted(na.items()):
        if name not in tools:
            problems.append(f"{name}: N/A entry for a tool that does not exist")
        elif not str(reason).strip():
            problems.append(f"{name}: N/A without a stated reason")
    covered = set(descriptions) | set(na)
    missing = sorted(set(tools) - covered)
    extra = sorted(covered - set(tools))
    if missing:
        problems.append(f"tools with neither a variant nor a stated N/A: {missing}")
    if extra:
        problems.append(f"entries for tools outside the live surface: {extra}")
    if set(descriptions) & set(na):
        problems.append("a tool is both described and marked N/A")
    if variant == "D":
        contrast = payload.get("neighbor_contrast") or {}
        if sorted(contrast) != sorted(descriptions):
            problems.append("every D description must name its contrast partner")
        for name, partner in sorted(contrast.items()):
            if partner not in tools:
                problems.append(f"{name}: contrast partner {partner!r} is not a tool")
            elif partner not in neighbors.get(name, set()):
                problems.append(
                    f"{name}: contrast partner {partner!r} is not a confusable "
                    f"neighbor of {name} in the E2.1 map")
            elif f"`{partner}`" not in descriptions.get(name, ""):
                problems.append(f"{name}: contrast partner {partner!r} is not "
                                f"named in the description text")
    return problems


class VariantFileShapeTest(unittest.TestCase):
    def setUp(self):
        self.tools = live_tools()
        self.neighbors = neighbor_map()

    def test_every_variant_set_is_legal(self):
        for key in VARIANT_FILES:
            with self.subTest(variant=key):
                payload = load_variant(key)
                self.assertEqual(payload["variant"], key)
                self.assertEqual(payload["version"], 1)
                self.assertEqual(
                    validate_variant(payload, self.tools, self.neighbors), [])

    def test_every_variant_set_covers_the_whole_live_surface(self):
        for key in VARIANT_FILES:
            with self.subTest(variant=key):
                payload = load_variant(key)
                covered = set(payload["descriptions"]) | set(payload.get("na") or {})
                self.assertEqual(sorted(covered), sorted(self.tools))

    def test_files_are_canonical_and_byte_stable(self):
        for key, path in VARIANT_FILES.items():
            with self.subTest(variant=key):
                payload = json.loads(path.read_text())
                self.assertEqual(
                    path.read_text(),
                    json.dumps(payload, indent=2, sort_keys=True,
                               ensure_ascii=False) + "\n")

    def test_validator_rejects_a_variant_that_carries_schema_material(self):
        """A variant may swap wording only; smuggled schema changes the experiment."""
        payload = load_variant("B")
        with_schema = dict(payload, input_schemas={"search": {"type": "object"}})
        self.assertTrue(validate_variant(with_schema, self.tools, self.neighbors))
        per_tool = dict(payload, descriptions=dict(
            payload["descriptions"],
            search={"description": "x", "input_schema": {"properties": {}}}))
        self.assertTrue(validate_variant(per_tool, self.tools, self.neighbors))

    def test_validator_rejects_an_invented_parameter(self):
        payload = load_variant("C")
        broken = dict(payload, descriptions=dict(
            payload["descriptions"],
            callers="Who calls it? Pass `regex_pattern` to filter."))
        problems = validate_variant(broken, self.tools, self.neighbors)
        self.assertTrue(any("regex_pattern" in p for p in problems))

    def test_validator_rejects_a_dropped_tool(self):
        payload = load_variant("B")
        pruned = dict(payload, descriptions={
            k: v for k, v in payload["descriptions"].items() if k != "search"})
        self.assertTrue(validate_variant(pruned, self.tools, self.neighbors))

    def test_validator_rejects_an_na_without_a_reason(self):
        payload = load_variant("B")
        blanked = dict(payload, descriptions={
            k: v for k, v in payload["descriptions"].items() if k != "info"},
            na={"info": "  "})
        self.assertTrue(validate_variant(blanked, self.tools, self.neighbors))


class VariantATest(unittest.TestCase):
    """A is the control arm: the descriptions LCI actually ships today."""

    def test_variant_a_is_the_manifest_description_verbatim(self):
        self.assertEqual(load_variant("A")["descriptions"], live_descriptions())

    def test_variant_a_equals_the_committed_baseline_arm(self):
        baseline = json.loads(BASELINE_DESCRIPTIONS.read_text())["descriptions"]
        self.assertEqual(load_variant("A")["descriptions"], baseline)

    def test_verbatim_gate_catches_a_reworded_a(self):
        reworded = dict(load_variant("A")["descriptions"],
                        search="Find text in the corpus.")
        self.assertNotEqual(reworded, live_descriptions())

    def test_variant_a_matches_the_live_tools_list(self):
        """Freshness: the committed A arm must still be what the binary serves.

        SKIPS with a named reason when the binary or corpus is absent -- it must
        never pass vacuously by treating absence as agreement.
        """
        if not LCI_BIN.exists():
            self.skipTest(
                f"variant A unverified against the live surface: no LCI binary at "
                f"{LCI_BIN}; build it (cmake --build build/release --target lci)")
        if not LIVE_CORPUS.is_dir():
            self.skipTest(
                f"variant A unverified against the live surface: no corpus at "
                f"{LIVE_CORPUS}; the server needs a directory to serve tools/list")
        import enumerate_tool_surface as surface
        session = surface.McpSession(LCI_BIN, LIVE_CORPUS)
        try:
            live = session.rpc("tools/list")["tools"]
        finally:
            session.close()
        served = {t["name"]: t.get("description", "") for t in live}
        self.assertEqual(served, load_variant("A")["descriptions"])


class AuthoredVariantContentTest(unittest.TestCase):
    def setUp(self):
        self.tools = live_tools()
        self.neighbors = neighbor_map()
        self.live = live_descriptions()

    def test_authored_variants_actually_differ_from_a(self):
        for key in AUTHORED:
            payload = load_variant(key)
            for name, text in payload["descriptions"].items():
                with self.subTest(variant=key, tool=name):
                    self.assertNotEqual(text, self.live[name])
                    self.assertTrue(text.strip())

    def test_variant_b_is_task_framed(self):
        for name, text in load_variant("B")["descriptions"].items():
            with self.subTest(tool=name):
                self.assertTrue(text.startswith("Use when you need "), name)

    def test_variant_c_carries_a_worked_invocation_over_real_parameters(self):
        for name, text in load_variant("C")["descriptions"].items():
            with self.subTest(tool=name):
                objects = _JSON_OBJECT.findall(text)
                self.assertTrue(objects, f"{name}: no worked invocation")
                args = json.loads(objects[-1])
                self.assertTrue(args, f"{name}: empty invocation")
                props = schema_properties(self.tools[name])
                self.assertLessEqual(set(args), props, f"{name}: invented parameters")
                required = set(
                    self.tools[name]["input_schema"].get("required") or [])
                self.assertLessEqual(
                    required, set(args),
                    f"{name}: worked invocation omits a required parameter")

    def test_variant_c_example_gate_catches_an_invented_parameter(self):
        props = schema_properties(self.tools["callers"])
        args = json.loads(_JSON_OBJECT.findall(
            'Example: {"name": "X", "regex": true}')[-1])
        self.assertFalse(set(args) <= props)

    def test_variant_d_contrasts_a_real_confusable_neighbor(self):
        payload = load_variant("D")
        for name, partner in payload["neighbor_contrast"].items():
            with self.subTest(tool=name):
                self.assertIn(partner, self.neighbors[name])
                self.assertIn("Unlike `%s`" % partner, payload["descriptions"][name])

    def test_variant_d_gate_catches_a_non_neighbor_contrast(self):
        payload = load_variant("D")
        victim = "callers"
        non_neighbor = next(t for t in self.tools
                            if t != victim and t not in self.neighbors[victim])
        broken = dict(payload,
                      neighbor_contrast=dict(payload["neighbor_contrast"],
                                             **{victim: non_neighbor}))
        problems = validate_variant(broken, self.tools, self.neighbors)
        self.assertTrue(any(non_neighbor in p for p in problems))


class VariantRenderingTest(unittest.TestCase):
    """Every variant set must actually load into the mock and render."""

    def setUp(self):
        import tooleval
        self.tooleval = tooleval
        self.tools = sorted(live_tools())

    def _listed(self, descriptions_path: Path) -> list[dict]:
        env = dict(os.environ, LCI_MOCK_DESCRIPTIONS=str(descriptions_path))
        session = self.tooleval.McpSession.__new__(self.tooleval.McpSession)
        session.proc = subprocess.Popen(
            [sys.executable, str(MOCK_SCRIPT), "mcp"], cwd=str(REPO_QA),
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, env=env)
        session.next_id = 1
        try:
            session._rpc("initialize", {
                "protocolVersion": "2025-06-18", "capabilities": {},
                "clientInfo": {"name": "e2.2-test", "version": "0"}})
            session._notify("notifications/initialized")
            return session._rpc("tools/list", {})["result"]["tools"]
        finally:
            if session.proc.poll() is None:
                session.close()
            session.proc.stdout.close()

    def test_every_variant_set_renders_through_the_mock(self):
        for key, path in VARIANT_FILES.items():
            with self.subTest(variant=key):
                listed = self._listed(path)
                self.assertEqual(
                    {t["name"]: t["description"] for t in listed},
                    load_variant(key)["descriptions"])

    def test_name_and_schema_are_constant_across_every_variant(self):
        """Only the description may move; anything else confounds the experiment."""
        shapes = {}
        for key, path in VARIANT_FILES.items():
            listed = self._listed(path)
            shapes[key] = sorted(
                (t["name"], json.dumps(t["inputSchema"], sort_keys=True))
                for t in listed)
        reference = shapes["A"]
        self.assertEqual([n for n, _ in reference], self.tools)
        for key, shape in shapes.items():
            with self.subTest(variant=key):
                self.assertEqual(shape, reference)


if __name__ == "__main__":
    unittest.main()
