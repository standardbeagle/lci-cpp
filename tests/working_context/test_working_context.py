"""Integration tests for the lci working-context Slop custom tools.

The suite drives the real ``slop-mcp`` interpreter against a controlled ``lci``
fixture MCP (the external boundary).  It fails — never skips — when the
interpreter is unavailable, when a custom-tool body does not parse, when an MCP
error envelope is swallowed, or when the installed tool schemas drift from the
shipped pack.
"""

import json
import os
import unittest

from harness import (
    CUSTOM_TOOL_NAMES,
    PACK_PATH,
    SlopSession,
    assert_no_parse_error,
    load_pack,
)


class PackShapeTest(unittest.TestCase):
    def test_pack_file_exists_and_is_schema_version_1(self):
        self.assertTrue(
            os.path.isfile(PACK_PATH),
            "shipped pack missing: %s" % PACK_PATH,
        )
        pack = load_pack()
        self.assertEqual(pack.get("schema_version"), 1)
        self.assertIn("custom_tools", pack)

    def test_pack_ships_exactly_the_six_operations(self):
        pack = load_pack()
        names = [t.get("name") for t in pack["custom_tools"]]
        self.assertEqual(sorted(names), sorted(CUSTOM_TOOL_NAMES))
        for tool in pack["custom_tools"]:
            self.assertTrue(tool.get("description"), tool.get("name"))
            self.assertEqual(tool.get("inputSchema", {}).get("type"), "object")
            self.assertTrue(tool.get("body", "").strip(), tool.get("name"))


class WorkingContextToolsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.pack = load_pack()
        cls.pack_tools = {
            t["name"]: t for t in cls.pack["custom_tools"]
        }
        cls.session = SlopSession()
        result = cls.session.import_pack(scope="project", overwrite=False)
        assert not result["is_error"], result["text"]
        listing = cls.session.list_custom(scope="project")["parsed"]
        installed = {e["name"] for e in listing.get("custom_entries", [])}
        missing = set(CUSTOM_TOOL_NAMES) - installed
        if missing:
            raise AssertionError("pack did not install tools: %s" % sorted(missing))

    @classmethod
    def tearDownClass(cls):
        cls.session.close()

    def setUp(self):
        self.session.set_control({})
        self.session.clear_calls()

    # -- installation --------------------------------------------------------

    def test_installed_schemas_match_shipped_pack(self):
        export = self.session.export_pack(scope="project")["parsed"]
        exported = {t["name"]: t for t in export["pack"]["custom_tools"]}
        for name in CUSTOM_TOOL_NAMES:
            self.assertIn(name, exported)
            self.assertEqual(
                exported[name]["inputSchema"],
                self.pack_tools[name]["inputSchema"],
                "schema drift for %s" % name,
            )
            self.assertEqual(exported[name]["body"], self.pack_tools[name]["body"])

    def test_import_with_overwrite_false_skips_existing(self):
        second = self.session.import_pack(scope="project", overwrite=False)
        parsed = second["parsed"]
        self.assertFalse(second["is_error"], second["text"])
        self.assertEqual(parsed.get("affected"), 0)
        self.assertEqual(
            sorted(parsed.get("import_report", {}).get("skipped", [])),
            sorted(CUSTOM_TOOL_NAMES),
        )

    def test_import_leaves_user_and_global_scope_untouched(self):
        user_listing = self.session.list_custom(scope="user")["parsed"]
        self.assertEqual(user_listing.get("affected"), 0)
        user_tools = os.path.join(
            self.session.xdg,
            "slop-mcp",
            "memory",
            "_slop",
            "_slop.tools.json",
        )
        self.assertFalse(
            os.path.exists(user_tools),
            "project import must not write personal/global tools: %s" % user_tools,
        )

    # -- save -----------------------------------------------------------------

    def _save(self, task, files, **extra):
        params = {"task": task, "files": files}
        params.update(extra)
        outcome = self.session.execute_custom("save_working_context", params)
        assert_no_parse_error(self, outcome, "save_working_context")
        return outcome

    def test_save_builds_compact_refs_preserving_roles_and_expand(self):
        files = [
            {
                "path": "src/a.cpp",
                "symbols": ["f", "g"],
                "role": "modify",
                "expand": ["callers"],
            },
            {"path": "src/b.cpp", "role": "contract"},
            {"path": "src/c.cpp", "symbols": ["h"], "role": "verify"},
        ]
        outcome = self._save("checkpoint", files)
        self.assertFalse(outcome["is_error"], outcome["text"])
        manifest = outcome["parsed"]
        self.assertEqual(manifest["t"], "checkpoint")
        self.assertEqual(manifest["v"], "1.0")
        refs = manifest["r"]
        self.assertEqual(
            refs,
            [
                {"f": "src/a.cpp", "s": "f", "role": "modify", "x": ["callers"]},
                {"f": "src/a.cpp", "s": "g", "role": "modify", "x": ["callers"]},
                {"f": "src/b.cpp", "role": "contract"},
                {"f": "src/c.cpp", "s": "h", "role": "verify"},
            ],
        )

    def test_save_contains_no_source(self):
        outcome = self._save(
            "manifest only",
            [{"path": "src/a.cpp", "symbols": ["f"], "role": "modify"}],
        )
        self.assertFalse(outcome["is_error"], outcome["text"])
        refs_json = json.dumps(outcome["parsed"]["r"]).lower()
        self.assertNotIn("source", refs_json)
        self.assertNotIn("excerpt", refs_json)
        for ref in outcome["parsed"]["r"]:
            self.assertLessEqual(set(ref.keys()), {"f", "s", "role", "x", "l", "n"})

    def test_save_preserves_unicode_and_windows_paths(self):
        files = [
            {"path": "src/ünïcödé.cpp", "symbols": ["héllo"], "role": "verify"},
            {"path": "C:\\win\\pa th.cpp", "symbols": ["WinFunc"]},
        ]
        outcome = self._save("encodings", files)
        self.assertFalse(outcome["is_error"], outcome["text"])
        refs = outcome["parsed"]["r"]
        self.assertEqual(refs[0]["f"], "src/ünïcödé.cpp")
        self.assertEqual(refs[0]["s"], "héllo")
        self.assertEqual(refs[1]["f"], "C:\\win\\pa th.cpp")
        self.assertEqual(refs[1]["s"], "WinFunc")

    # -- open -----------------------------------------------------------------

    def test_open_performs_one_shared_budget_load(self):
        self.session.set_control({"index": {"src/a.cpp": ["f"]}})
        files = [
            {"path": "src/a.cpp", "symbols": ["f"], "role": "modify"},
            {"path": "src/b.cpp", "role": "contract"},
        ]
        outcome = self.session.execute_custom(
            "open_files_context", {"task": "open", "files": files}
        )
        assert_no_parse_error(self, outcome, "open_files_context")
        self.assertFalse(outcome["is_error"], outcome["text"])
        context_calls = self.session.context_calls()
        self.assertEqual(len(context_calls), 1, context_calls)
        call = context_calls[0]
        self.assertEqual(call["args"]["operation"], "load")
        self.assertEqual(call["args"]["max_tokens"], 8000)
        sent = json.loads(call["args"]["from_string"])
        self.assertEqual(sent["t"], "open")
        self.assertEqual(sent["v"], "1.0")
        self.assertEqual(sent["r"], [{"f": "src/a.cpp", "s": "f", "role": "modify"},
                                     {"f": "src/b.cpp", "role": "contract"}])
        self.assertNotIn("source", call["args"]["from_string"].lower())
        self.assertEqual(outcome["parsed"]["budget"], 8000)

    def test_open_honours_explicit_max_tokens(self):
        self.session.set_control({"index": {"src/a.cpp": ["f"]}})
        outcome = self.session.execute_custom(
            "open_files_context",
            {
                "task": "open",
                "files": [{"path": "src/a.cpp", "symbols": ["f"]}],
                "max_tokens": 1234,
            },
        )
        self.assertFalse(outcome["is_error"], outcome["text"])
        call = self.session.context_calls()[0]
        self.assertEqual(call["args"]["max_tokens"], 1234)
        self.assertEqual(outcome["parsed"]["budget"], 1234)

    def test_open_missing_host_files_report_unresolved_without_error(self):
        self.session.set_control({"index": {}})
        outcome = self.session.execute_custom(
            "open_files_context",
            {
                "task": "missing",
                "files": [
                    {"path": "src/gone.cpp", "symbols": ["nope"]},
                    {"path": "src/also_gone.cpp"},
                ],
            },
        )
        assert_no_parse_error(self, outcome, "open_files_context")
        self.assertFalse(outcome["is_error"], outcome["text"])
        context = outcome["parsed"]
        self.assertEqual(context["refs"], [])
        self.assertEqual(len(context["unresolved"]), 2)

    # -- resume / refresh -----------------------------------------------------

    def test_resume_loads_supplied_manifest(self):
        self.session.set_control({"index": {"src/a.cpp": ["f"]}})
        manifest = json.dumps(
            {
                "t": "resume",
                "v": "1.0",
                "r": [{"f": "src/a.cpp", "s": "f", "role": "modify"}],
            }
        )
        outcome = self.session.execute_custom(
            "resume_working_context", {"manifest": manifest, "max_tokens": 999}
        )
        assert_no_parse_error(self, outcome, "resume_working_context")
        self.assertFalse(outcome["is_error"], outcome["text"])
        call = self.session.context_calls()[0]
        self.assertEqual(call["args"]["operation"], "load")
        self.assertEqual(call["args"]["from_string"], manifest)
        self.assertEqual(call["args"]["max_tokens"], 999)
        self.assertEqual(outcome["parsed"]["refs"][0]["symbol"], "f")

    def test_refresh_reresolves_references_and_drops_cached_fields(self):
        self.session.set_control({"index": {"src/a.cpp": ["kept"]}})
        manifest = json.dumps(
            {
                "t": "refresh",
                "v": "1.0",
                "r": [
                    {
                        "f": "src/a.cpp",
                        "s": "kept",
                        "role": "modify",
                        "source": "MUST-NOT-SURVIVE",
                    },
                    {"f": "src/a.cpp", "s": "deleted", "role": "contract"},
                ],
                "p": "/stale/root",
                "s": {"rc": 2},
            }
        )
        outcome = self.session.execute_custom(
            "refresh_working_context", {"manifest": manifest}
        )
        assert_no_parse_error(self, outcome, "refresh_working_context")
        self.assertFalse(outcome["is_error"], outcome["text"])
        call = self.session.context_calls()[0]
        sent = json.loads(call["args"]["from_string"])
        self.assertEqual(
            sent["r"],
            [
                {"f": "src/a.cpp", "s": "kept", "role": "modify"},
                {"f": "src/a.cpp", "s": "deleted", "role": "contract"},
            ],
        )
        self.assertNotIn("source", call["args"]["from_string"].lower())
        self.assertNotIn("stale/root", call["args"]["from_string"])
        context = outcome["parsed"]
        self.assertEqual(context["refs"][0]["symbol"], "kept")
        self.assertEqual(context["unresolved"][0]["symbol"], "deleted")

    # -- visible rejection ----------------------------------------------------

    def _assert_rejected(self, files, substring, tool="save_working_context"):
        outcome = self.session.execute_custom(tool, {"task": "t", "files": files})
        self.assertTrue(outcome["is_error"], "expected rejection: %r" % files)
        self.assertIn(substring, outcome["text"])
        self.assertNotIn("parse error", outcome["text"].lower())

    def test_empty_working_set_rejected(self):
        self._assert_rejected([], "empty working set")

    def test_malformed_entries_rejected(self):
        self._assert_rejected(["not-an-object"], "must be an object")
        self._assert_rejected([{"symbols": ["f"]}], "path")
        self._assert_rejected([{"path": "a.cpp", "symbols": "f"}], "symbols must be a list")
        self._assert_rejected([{"path": "a.cpp", "symbols": [1]}], "symbol must be")
        self._assert_rejected([{"path": "a.cpp", "role": 1}], "role must be a string")
        self._assert_rejected([{"path": "a.cpp", "expand": "callers"}], "expand must be a list")
        self._assert_rejected([{"path": "a.cpp", "expand": [1]}], "expand entries must be strings")

    def test_contradictory_working_set_rejected(self):
        self._assert_rejected(
            [{"path": "a.cpp"}, {"path": "a.cpp"}], "duplicate path"
        )
        self._assert_rejected(
            [{"path": "a.cpp", "symbols": ["f", "f"]}], "duplicate symbol"
        )

    def test_more_than_128_refs_rejected(self):
        too_many = {"path": "a.cpp", "symbols": ["s%d" % i for i in range(129)]}
        outcome = self.session.execute_custom(
            "save_working_context", {"task": "t", "files": [too_many]}
        )
        self.assertTrue(outcome["is_error"])
        self.assertIn("128", outcome["text"])

    def test_exactly_128_refs_accepted(self):
        exactly = {"path": "a.cpp", "symbols": ["s%d" % i for i in range(128)]}
        outcome = self.session.execute_custom(
            "save_working_context", {"task": "t", "files": [exactly]}
        )
        self.assertFalse(outcome["is_error"], outcome["text"])
        self.assertEqual(len(outcome["parsed"]["r"]), 128)

    def test_bad_max_tokens_rejected(self):
        outcome = self.session.execute_custom(
            "save_working_context",
            {"task": "t", "files": [{"path": "a.cpp"}], "max_tokens": 0},
        )
        self.assertTrue(outcome["is_error"])
        self.assertIn("max_tokens", outcome["text"])

    # -- transport error surfaces --------------------------------------------

    def test_mcp_error_envelope_propagates(self):
        self.session.set_control(
            {"load_is_error": True, "load_error": "index not available"}
        )
        outcome = self.session.execute_custom(
            "open_files_context",
            {"task": "t", "files": [{"path": "a.cpp"}]},
        )
        self.assertTrue(outcome["is_error"], outcome["text"])
        self.assertIn("index not available", outcome["text"])

    def test_save_mcp_error_propagates(self):
        self.session.set_control(
            {"save_is_error": True, "save_error": "manifest rejected"}
        )
        outcome = self.session.execute_custom(
            "save_working_context",
            {"task": "t", "files": [{"path": "a.cpp"}]},
        )
        self.assertTrue(outcome["is_error"], outcome["text"])
        self.assertIn("manifest rejected", outcome["text"])

    def test_indexing_unavailable_is_surfaced(self):
        self.session.set_control(
            {
                "load_unavailable": True,
                "unavailable_reason": "indexing in progress",
                "unavailable_hint": "retry shortly",
            }
        )
        outcome = self.session.execute_custom(
            "open_files_context",
            {"task": "t", "files": [{"path": "a.cpp"}]},
        )
        self.assertTrue(outcome["is_error"], outcome["text"])
        self.assertIn("indexing in progress", outcome["text"])

    def test_resume_bad_manifest_rejected(self):
        outcome = self.session.execute_custom(
            "resume_working_context", {"manifest": "{not json"}
        )
        self.assertTrue(outcome["is_error"])
        self.assertIn("JSON", outcome["text"])

    def test_resume_empty_manifest_rejected(self):
        outcome = self.session.execute_custom(
            "resume_working_context", {"manifest": ""}
        )
        self.assertTrue(outcome["is_error"])
        self.assertIn("manifest", outcome["text"])


def group(file, hits):
    return {"file": file, "hits": hits}


def hit(line, sym, score, type="function", id=None):
    row = {"line": line, "sym": sym, "type": type, "score": score}
    if id is not None:
        row["id"] = id
    return row


class DiscoverContextTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.session = SlopSession()
        result = cls.session.import_pack(scope="project", overwrite=False)
        assert not result["is_error"], result["text"]

    @classmethod
    def tearDownClass(cls):
        cls.session.close()

    def setUp(self):
        self.session.set_control({})
        self.session.clear_calls()

    def _discover(self, terms, **extra):
        params = {"terms": terms}
        params.update(extra)
        outcome = self.session.execute_custom("discover_context", params)
        assert_no_parse_error(self, outcome, "discover_context")
        return outcome

    def test_overlapping_terms_dedupe_by_file_symbol_and_keep_max_score(self):
        self.session.set_control(
            {
                "index": {"src/a.cpp": ["Alpha"], "src/b.cpp": ["Beta"]},
                "searches": {
                    "alpha": {
                        "total_matches": 2,
                        "results": [
                            group("src/a.cpp", [hit(10, "Alpha", 9, id="A1")]),
                            group("src/b.cpp", [hit(20, "Beta", 5, id="B1")]),
                        ],
                    },
                    "beta": {
                        "total_matches": 1,
                        "results": [
                            group("src/b.cpp", [hit(20, "Beta", 8, id="B1")]),
                        ],
                    },
                },
            }
        )
        outcome = self._discover(["alpha", "beta"])
        self.assertFalse(outcome["is_error"], outcome["text"])
        parsed = outcome["parsed"]
        self.assertEqual(
            parsed["discover"]["selected"],
            [
                {"f": "src/a.cpp", "s": "Alpha", "score": 9},
                {"f": "src/b.cpp", "s": "Beta", "score": 8},
            ],
        )
        self.assertEqual(parsed["discover"]["candidates"], 2)
        # exactly one search per term, exactly one shared load
        self.assertEqual(len(self.session.search_calls()), 2)
        loads = self.session.context_calls()
        self.assertEqual(len(loads), 1, loads)
        sent = json.loads(loads[0]["args"]["from_string"])
        self.assertEqual(
            sent["r"],
            [{"f": "src/a.cpp", "s": "Alpha"}, {"f": "src/b.cpp", "s": "Beta"}],
        )
        self.assertNotIn("source", loads[0]["args"]["from_string"].lower())

    def test_deterministic_ties_break_by_file_symbol_line(self):
        self.session.set_control(
            {
                "index": {
                    "src/a.cpp": ["Zed"],
                    "src/b.cpp": ["Beta"],
                    "src/c.cpp": ["Mid"],
                },
                "searches": {
                    "t": {
                        "total_matches": 3,
                        "results": [
                            group("src/b.cpp", [hit(20, "Beta", 5, id="B1")]),
                            group("src/a.cpp", [hit(30, "Zed", 5, id="A1")]),
                            group("src/c.cpp", [hit(10, "Mid", 5, id="C1")]),
                        ],
                    }
                },
            }
        )
        first = self._discover(["t"])
        order = [(s["f"], s["s"], s["score"]) for s in first["parsed"]["discover"]["selected"]]
        self.assertEqual(
            order,
            [("src/a.cpp", "Zed", 5), ("src/b.cpp", "Beta", 5), ("src/c.cpp", "Mid", 5)],
        )
        self.session.clear_calls()
        second = self._discover(["t"])
        order2 = [(s["f"], s["s"], s["score"]) for s in second["parsed"]["discover"]["selected"]]
        self.assertEqual(order, order2)

    def test_duplicate_symbol_names_across_files_stay_distinct(self):
        self.session.set_control(
            {
                "index": {"src/a.cpp": ["Dup"], "src/b.cpp": ["Dup"]},
                "searches": {
                    "dup": {
                        "total_matches": 2,
                        "results": [
                            group("src/a.cpp", [hit(1, "Dup", 7, id="A1")]),
                            group("src/b.cpp", [hit(2, "Dup", 7, id="B1")]),
                        ],
                    }
                },
            }
        )
        outcome = self._discover(["dup"])
        self.assertEqual(
            outcome["parsed"]["discover"]["selected"],
            [
                {"f": "src/a.cpp", "s": "Dup", "score": 7},
                {"f": "src/b.cpp", "s": "Dup", "score": 7},
            ],
        )

    def test_file_filters_restrict_candidates(self):
        self.session.set_control(
            {
                "index": {"src/a.cpp": ["Alpha"], "src/b.cpp": ["Beta"]},
                "searches": {
                    "t": {
                        "total_matches": 2,
                        "results": [
                            group("src/a.cpp", [hit(1, "Alpha", 3, id="A1")]),
                            group("src/b.cpp", [hit(2, "Beta", 9, id="B1")]),
                        ],
                    }
                },
            }
        )
        outcome = self._discover(["t"], files=["src/b.cpp"])
        self.assertEqual(
            outcome["parsed"]["discover"]["selected"],
            [{"f": "src/b.cpp", "s": "Beta", "score": 9}],
        )

    def test_no_matches_is_explicit_and_skips_load(self):
        self.session.set_control({"searches": {"nothing": {"results": [], "total_matches": 0}}})
        outcome = self._discover(["nothing"])
        self.assertFalse(outcome["is_error"], outcome["text"])
        parsed = outcome["parsed"]
        self.assertTrue(parsed["discover"]["empty"])
        self.assertEqual(parsed["discover"]["selected"], [])
        self.assertEqual(self.session.context_calls(), [])

    def test_one_failed_search_is_reported_and_others_hydrate(self):
        self.session.set_control(
            {
                "index": {"src/a.cpp": ["Alpha"]},
                "searches": {
                    "good": {
                        "total_matches": 1,
                        "results": [group("src/a.cpp", [hit(1, "Alpha", 4, id="A1")])],
                    },
                    "bad": {"error": "search backend exploded"},
                },
            }
        )
        outcome = self._discover(["good", "bad"])
        self.assertFalse(outcome["is_error"], outcome["text"])
        parsed = outcome["parsed"]
        errors = parsed["discover"]["search_errors"]
        self.assertEqual(len(errors), 1)
        self.assertEqual(errors[0]["term"], "bad")
        self.assertIn("search backend exploded", errors[0]["error"])
        self.assertEqual(
            parsed["discover"]["selected"], [{"f": "src/a.cpp", "s": "Alpha", "score": 4}]
        )
        self.assertEqual(len(self.session.context_calls()), 1)

    def test_search_failure_for_every_term_is_explicit(self):
        self.session.set_control({"search_is_error": True, "search_error": "index down"})
        outcome = self._discover(["a", "b"])
        self.assertFalse(outcome["is_error"], outcome["text"])
        errors = outcome["parsed"]["discover"]["search_errors"]
        self.assertEqual(len(errors), 2)
        self.assertEqual(self.session.context_calls(), [])

    def test_candidate_cap_hydrates_at_most_sixteen(self):
        hits = [hit(i + 1, "S%02d" % i, 100 - i, id="S%02d" % i) for i in range(20)]
        self.session.set_control(
            {
                "index": {"src/big.cpp": ["S%02d" % i for i in range(20)]},
                "searches": {
                    "big": {"total_matches": 20, "results": [group("src/big.cpp", hits)]}
                },
            }
        )
        outcome = self._discover(["big"])
        parsed = outcome["parsed"]
        self.assertEqual(len(parsed["discover"]["selected"]), 16)
        self.assertEqual(parsed["discover"]["omitted"], 4)
        loads = self.session.context_calls()
        self.assertEqual(len(loads), 1)
        sent = json.loads(loads[0]["args"]["from_string"])
        self.assertEqual(len(sent["r"]), 16)

    def test_shared_budget_covers_source_and_ranking_metadata(self):
        self.session.set_control(
            {
                "index": {"src/a.cpp": ["Alpha"]},
                "searches": {
                    "alpha": {
                        "total_matches": 1,
                        "results": [group("src/a.cpp", [hit(1, "Alpha", 4, id="A1")])],
                    }
                },
            }
        )
        outcome = self._discover(["alpha"], max_tokens=200)
        self.assertFalse(outcome["is_error"], outcome["text"])
        budget = outcome["parsed"]["discover"]["budget"]
        self.assertEqual(budget["requested"], 200)
        self.assertLess(budget["loader"], 200)
        self.assertGreater(budget["loader"], 0)
        self.assertEqual(budget["loader"] + budget["ranking"], 200)
        loads = self.session.context_calls()
        self.assertEqual(loads[0]["args"]["max_tokens"], budget["loader"])

    def test_eight_terms_run_eight_searches_and_one_load(self):
        terms = ["t%d" % i for i in range(8)]
        searches = {
            t: {
                "total_matches": 1,
                "results": [group("src/a.cpp", [hit(1, "Alpha", 4, id="A1")])],
            }
            for t in terms
        }
        self.session.set_control(
            {"index": {"src/a.cpp": ["Alpha"]}, "searches": searches}
        )
        outcome = self._discover(terms)
        self.assertFalse(outcome["is_error"], outcome["text"])
        self.assertEqual(len(self.session.search_calls()), 8)
        self.assertEqual(len(self.session.context_calls()), 1)
        self.assertEqual(
            outcome["parsed"]["discover"]["selected"],
            [{"f": "src/a.cpp", "s": "Alpha", "score": 4}],
        )

    def test_impossible_budget_is_rejected(self):
        self.session.set_control(
            {
                "searches": {
                    "alpha": {
                        "total_matches": 1,
                        "results": [group("src/a.cpp", [hit(1, "Alpha", 4, id="A1")])],
                    }
                }
            }
        )
        outcome = self._discover(["alpha"], max_tokens=1)
        self.assertTrue(outcome["is_error"], outcome["text"])
        self.assertIn("max_tokens", outcome["text"])
        self.assertEqual(self.session.context_calls(), [])

    def test_terms_validated_between_one_and_eight(self):
        self.assertIn("terms", self._discover([])["text"])
        self.assertIn("terms", self._discover(["t%d" % i for i in range(9)])["text"])
        self.assertIn("terms", self._discover([""])["text"])
        self.assertIn("terms", self._discover(["a", "a"])["text"])

    def test_load_error_envelope_propagates(self):
        self.session.set_control(
            {
                "load_is_error": True,
                "load_error": "index not available",
                "searches": {
                    "alpha": {
                        "total_matches": 1,
                        "results": [group("src/a.cpp", [hit(1, "Alpha", 4, id="A1")])],
                    }
                },
            }
        )
        outcome = self._discover(["alpha"])
        self.assertTrue(outcome["is_error"], outcome["text"])
        self.assertIn("index not available", outcome["text"])


class TraceContextTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.session = SlopSession()
        result = cls.session.import_pack(scope="project", overwrite=False)
        assert not result["is_error"], result["text"]

    @classmethod
    def tearDownClass(cls):
        cls.session.close()

    def setUp(self):
        self.session.set_control({})
        self.session.clear_calls()

    def _trace(self, file, symbol, **extra):
        params = {"file": file, "symbol": symbol}
        params.update(extra)
        outcome = self.session.execute_custom("trace_context", params)
        assert_no_parse_error(self, outcome, "trace_context")
        return outcome

    def test_trace_requests_all_evidence_through_the_shared_loader(self):
        self.session.set_control({"index": {"src/a.cpp": ["Alpha"]}})
        outcome = self._trace("src/a.cpp", "Alpha")
        self.assertFalse(outcome["is_error"], outcome["text"])
        loads = self.session.context_calls()
        self.assertEqual(len(loads), 1, loads)
        sent = json.loads(loads[0]["args"]["from_string"])
        self.assertEqual(
            sent["r"],
            [
                {
                    "f": "src/a.cpp",
                    "s": "Alpha",
                    "x": ["callers", "references", "tests", "side_effects"],
                }
            ],
        )
        parsed = outcome["parsed"]
        self.assertTrue(parsed["trace"]["resolved"])
        self.assertEqual(parsed["refs"][0]["symbol"], "Alpha")

    def test_trace_ambiguity_is_unresolved_not_first_match(self):
        self.session.set_control(
            {"ambiguous": [{"file": "src/a.cpp", "symbol": "Same"}]}
        )
        outcome = self._trace("src/a.cpp", "Same")
        self.assertFalse(outcome["is_error"], outcome["text"])
        parsed = outcome["parsed"]
        self.assertFalse(parsed["trace"]["resolved"])
        self.assertEqual(parsed["unresolved"][0]["reason"], "ambiguous_symbol")
        loads = self.session.context_calls()
        sent = json.loads(loads[0]["args"]["from_string"])
        self.assertEqual(len(sent["r"]), 1)

    def test_trace_missing_symbol_is_unresolved(self):
        self.session.set_control({"index": {}})
        outcome = self._trace("src/a.cpp", "Gone")
        self.assertFalse(outcome["is_error"], outcome["text"])
        parsed = outcome["parsed"]
        self.assertFalse(parsed["trace"]["resolved"])
        self.assertEqual(parsed["unresolved"][0]["reason"], "not_found")

    def test_trace_shared_budget_covers_source_and_metadata(self):
        self.session.set_control({"index": {"src/a.cpp": ["Alpha"]}})
        outcome = self._trace("src/a.cpp", "Alpha", max_tokens=150)
        budget = outcome["parsed"]["trace"]["budget"]
        self.assertEqual(budget["requested"], 150)
        self.assertLess(budget["loader"], 150)
        self.assertGreater(budget["loader"], 0)
        self.assertEqual(budget["loader"] + budget["ranking"], 150)
        loads = self.session.context_calls()
        self.assertEqual(loads[0]["args"]["max_tokens"], budget["loader"])

    def test_trace_requires_file_and_symbol(self):
        self.assertTrue(self._trace("", "Alpha")["is_error"])
        self.assertTrue(self._trace("src/a.cpp", "")["is_error"])
        self.assertEqual(self.session.context_calls(), [])

    def test_trace_load_error_envelope_propagates(self):
        self.session.set_control(
            {"load_is_error": True, "load_error": "index not available"}
        )
        outcome = self._trace("src/a.cpp", "Alpha")
        self.assertTrue(outcome["is_error"], outcome["text"])
        self.assertIn("index not available", outcome["text"])


if __name__ == "__main__":
    unittest.main()
