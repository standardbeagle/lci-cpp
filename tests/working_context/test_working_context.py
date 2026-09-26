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

    def test_pack_ships_exactly_the_four_operations(self):
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


if __name__ == "__main__":
    unittest.main()
