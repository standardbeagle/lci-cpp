"""Hermetic tests for the deterministic exploration corpus forge.

Every test builds a tiny synthetic git repo in a temp dir. The three real
corpora (scikit-learn, PocketBase, next.js) are NEVER cloned or mutated here:
they are large, slow, and read-only pinned sources. The forge treats them
exactly like these fixtures, so fixture coverage is the real coverage.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

SCRIPTS = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "scripts"
)
sys.path.insert(0, SCRIPTS)

import exploration_corpus_forge as forge  # noqa: E402


def git(cwd, *args):
    subprocess.run(
        ["git", *args],
        cwd=cwd,
        check=True,
        capture_output=True,
        text=True,
    )


# A tiny python "project": two packages, absolute cross-package imports, and a
# donor module with top-level symbols the forge can twin into a decoy.
FIXTURE_FILES = {
    "alpha/__init__.py": "from alpha.core import Engine\n",
    "alpha/core.py": (
        "import alpha.util\n"
        "\n"
        "\n"
        "class Engine:\n"
        "    def run(self):\n"
        "        return alpha.util.helper()\n"
    ),
    "alpha/util.py": "def helper():\n    return 1\n",
    "beta/__init__.py": "",
    "beta/client.py": (
        "from alpha.core import Engine\n"
        "\n"
        "\n"
        "def make():\n"
        "    return Engine()\n"
    ),
    "beta/models.py": "class Record:\n    pass\n",
    "docs/guide.md": "See `alpha/core.py` and the alpha.util module.\n",
}


def make_fixture_repo(root):
    for rel, body in FIXTURE_FILES.items():
        path = os.path.join(root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write(body)
    git(root, "init", "-q")
    git(root, "config", "user.email", "fixture@example.com")
    git(root, "config", "user.name", "Fixture")
    git(root, "add", "-A")
    git(root, "commit", "-q", "-m", "fixture")
    out = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=root,
        check=True,
        capture_output=True,
        text=True,
    )
    return out.stdout.strip()


def spec_for(source_path, commit, validation=None):
    return {
        "id": "fixture",
        "source_path": source_path,
        "pinned_commit": commit,
        "language": "python",
        "source_extensions": [".py"],
        "mutable_roots": ["alpha", "beta"],
        "protect": [],
        "reference_forms": ["slashed", "dotted"],
        "symbol_pattern": r"^(?:def|class)\s+(\w+)",
        "no_shuffle_pattern": r"^\s*from\s+\.",
        "mutations": {"dir_renames": 1, "module_shuffles": 1, "decoys": 2},
        "validation": validation
        or {"argv": ["{python}", "{forge}", "check-imports", "--language", "python"]},
    }


class ForgeTestCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="forge-test-")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.source = os.path.join(self.tmp, "source")
        os.makedirs(self.source)
        self.commit = make_fixture_repo(self.source)

    def out(self, name):
        return os.path.join(self.tmp, "out", name)

    def forge(self, seed=7, spec=None, **kw):
        spec = spec or spec_for(self.source, self.commit)
        return forge.forge_corpus(spec, seed=seed, out_dir=self.out(f"s{seed}"), **kw)


class TestDeterminism(ForgeTestCase):
    def test_same_seed_is_byte_identical(self):
        """Same source commit + forge version + seed => identical bytes."""
        a = forge.forge_corpus(
            spec_for(self.source, self.commit), seed=7, out_dir=self.out("a")
        )
        b = forge.forge_corpus(
            spec_for(self.source, self.commit), seed=7, out_dir=self.out("b")
        )

        with open(os.path.join(self.out("a"), "manifest.json"), "rb") as f:
            bytes_a = f.read()
        with open(os.path.join(self.out("b"), "manifest.json"), "rb") as f:
            bytes_b = f.read()

        self.assertEqual(bytes_a, bytes_b, "manifests must be byte-identical")
        self.assertEqual(a["tree_hash"], b["tree_hash"])
        self.assertEqual(a["mutations"], b["mutations"])

    def test_rerun_into_same_dir_is_idempotent(self):
        first = self.forge(seed=7)
        second = self.forge(seed=7)
        self.assertEqual(first["tree_hash"], second["tree_hash"])


class TestSeedChange(ForgeTestCase):
    def test_different_seed_diverges_but_mapping_stays_valid(self):
        a = forge.forge_corpus(
            spec_for(self.source, self.commit), seed=7, out_dir=self.out("a")
        )
        b = forge.forge_corpus(
            spec_for(self.source, self.commit), seed=99, out_dir=self.out("b")
        )

        self.assertNotEqual(a["tree_hash"], b["tree_hash"])
        self.assertNotEqual(a["mutations"], b["mutations"])

        # Both keep a usable original -> mutated anchor translation.
        for manifest, out in ((a, self.out("a")), (b, self.out("b"))):
            for original in ("alpha/core.py", "alpha/util.py", "beta/client.py"):
                mutated = forge.translate_path(manifest, original)
                self.assertTrue(
                    os.path.isfile(os.path.join(out, "tree", mutated)),
                    f"{original} -> {mutated} must exist in the mutated tree",
                )

    def test_translate_path_is_identity_for_untouched_files(self):
        manifest = self.forge(seed=7)
        self.assertEqual(
            forge.translate_path(manifest, "docs/guide.md"), "docs/guide.md"
        )

    def test_translate_path_rejects_unknown_original(self):
        manifest = self.forge(seed=7)
        with self.assertRaises(forge.ForgeError):
            forge.translate_path(manifest, "alpha/nope.py")


class TestManifestContents(ForgeTestCase):
    def test_manifest_records_every_required_field(self):
        manifest = self.forge(seed=7)
        for field in (
            "corpus_id",
            "source_commit",
            "seed",
            "forge_version",
            "mutations",
            "path_map",
            "decoys",
            "tree_hash",
            "validation",
            "status",
        ):
            self.assertIn(field, manifest)

        self.assertEqual(manifest["corpus_id"], "fixture")
        self.assertEqual(manifest["source_commit"], self.commit)
        self.assertEqual(manifest["seed"], 7)
        self.assertEqual(manifest["forge_version"], forge.FORGE_VERSION)
        self.assertEqual(manifest["status"], "ready")

        # Mutation ops are ordered and only of the three sanctioned kinds.
        kinds = {op["kind"] for op in manifest["mutations"]}
        self.assertTrue(
            kinds <= {"dir_rename", "module_shuffle", "decoy_module"}, kinds
        )
        self.assertEqual(
            [op["index"] for op in manifest["mutations"]],
            list(range(len(manifest["mutations"]))),
        )
        self.assertTrue(manifest["path_map"])
        self.assertTrue(manifest["validation"]["passed"])
        self.assertEqual(manifest["validation"]["exit_code"], 0)

    def test_ready_marker_written_on_success(self):
        self.forge(seed=7)
        self.assertTrue(os.path.isfile(os.path.join(self.out("s7"), "READY")))


class TestDecoyProvenance(ForgeTestCase):
    def test_decoys_record_donor_and_twinned_symbols(self):
        manifest = self.forge(seed=7)
        decoys = manifest["decoys"]
        self.assertEqual(len(decoys), 2)

        tree = os.path.join(self.out("s7"), "tree")
        for decoy in decoys:
            self.assertIn("path", decoy)
            self.assertIn("derived_from", decoy)
            self.assertIn("symbols", decoy)
            self.assertEqual(decoy["source_commit"], self.commit)

            # The donor is a real file of the ORIGINAL tree...
            self.assertIn(decoy["derived_from"], FIXTURE_FILES)
            # ...the decoy is a real file of the MUTATED tree...
            self.assertTrue(os.path.isfile(os.path.join(tree, decoy["path"])))
            # ...and is not itself part of the original tree.
            self.assertNotIn(decoy["path"], FIXTURE_FILES)

            # Twinned symbols are renamed, so they cannot collide with the donor.
            self.assertTrue(decoy["symbols"])
            with open(os.path.join(tree, decoy["path"])) as f:
                body = f.read()
            for symbol in decoy["symbols"]:
                self.assertIn(symbol["twin"], body)
                self.assertNotEqual(symbol["twin"], symbol["original"])

    def test_decoys_are_dead_nothing_references_them(self):
        manifest = self.forge(seed=7)
        tree = os.path.join(self.out("s7"), "tree")
        decoy_paths = {d["path"] for d in manifest["decoys"]}

        for root, _dirs, files in os.walk(tree):
            for name in files:
                path = os.path.join(root, name)
                rel = os.path.relpath(path, tree)
                if rel in decoy_paths:
                    continue
                with open(path, errors="replace") as f:
                    body = f.read()
                for decoy in manifest["decoys"]:
                    stem = os.path.splitext(os.path.basename(decoy["path"]))[0]
                    self.assertNotIn(
                        stem, body, f"{rel} must not reference decoy {decoy['path']}"
                    )


class TestSourceGuards(ForgeTestCase):
    def test_dirty_source_is_refused(self):
        with open(os.path.join(self.source, "alpha", "util.py"), "a") as f:
            f.write("# dirty\n")

        with self.assertRaises(forge.ForgeError) as ctx:
            self.forge(seed=7)
        self.assertIn("dirty", str(ctx.exception).lower())

    def test_dirty_source_is_allowed_with_explicit_override(self):
        with open(os.path.join(self.source, "alpha", "util.py"), "a") as f:
            f.write("# dirty\n")

        manifest = self.forge(seed=7, allow_dirty=True)
        self.assertEqual(manifest["status"], "ready")
        self.assertEqual(manifest["overrides"], ["allow_dirty"])

    def test_untracked_file_makes_source_dirty(self):
        with open(os.path.join(self.source, "stray.py"), "w") as f:
            f.write("x = 1\n")
        with self.assertRaises(forge.ForgeError):
            self.forge(seed=7)

    def test_head_mismatch_is_refused(self):
        spec = spec_for(self.source, self.commit)
        with open(os.path.join(self.source, "alpha", "util.py"), "a") as f:
            f.write("# moved on\n")
        git(self.source, "commit", "-qam", "move HEAD off the pin")

        with self.assertRaises(forge.ForgeError) as ctx:
            forge.forge_corpus(spec, seed=7, out_dir=self.out("a"))
        self.assertIn("HEAD", str(ctx.exception))

    def test_head_mismatch_allowed_with_explicit_override(self):
        spec = spec_for(self.source, self.commit)
        with open(os.path.join(self.source, "alpha", "util.py"), "a") as f:
            f.write("# moved on\n")
        git(self.source, "commit", "-qam", "move HEAD off the pin")

        manifest = forge.forge_corpus(
            spec, seed=7, out_dir=self.out("a"), allow_head_mismatch=True
        )
        # The PINNED commit is what gets forged, not HEAD.
        self.assertEqual(manifest["source_commit"], self.commit)
        self.assertEqual(manifest["overrides"], ["allow_head_mismatch"])

    def test_source_checkout_is_never_modified(self):
        before = subprocess.run(
            ["git", "status", "--porcelain"],
            cwd=self.source,
            capture_output=True,
            text=True,
        ).stdout
        self.forge(seed=7)
        after = subprocess.run(
            ["git", "status", "--porcelain"],
            cwd=self.source,
            capture_output=True,
            text=True,
        ).stdout
        self.assertEqual(before, after, "forge must never touch the source checkout")


class TestValidation(ForgeTestCase):
    def test_validation_failure_is_never_marked_ready(self):
        spec = spec_for(
            self.source,
            self.commit,
            validation={"argv": ["{python}", "-c", "import sys; sys.exit(3)"]},
        )

        with self.assertRaises(forge.ValidationFailed) as ctx:
            forge.forge_corpus(spec, seed=7, out_dir=self.out("bad"))
        self.assertEqual(ctx.exception.manifest["status"], "validation_failed")
        self.assertEqual(ctx.exception.manifest["validation"]["exit_code"], 3)
        self.assertFalse(ctx.exception.manifest["validation"]["passed"])

        # The failure is still forensically recorded, but never marked ready.
        with open(os.path.join(self.out("bad"), "manifest.json")) as f:
            written = json.load(f)
        self.assertEqual(written["status"], "validation_failed")
        self.assertFalse(os.path.isfile(os.path.join(self.out("bad"), "READY")))

    def test_mutating_validation_is_rejected(self):
        """A validation command that writes into the tree is not a validation."""
        spec = spec_for(
            self.source,
            self.commit,
            validation={
                "argv": [
                    "{python}",
                    "-c",
                    "open('side-effect.txt', 'w').write('x')",
                ]
            },
        )
        with self.assertRaises(forge.ForgeError) as ctx:
            forge.forge_corpus(spec, seed=7, out_dir=self.out("bad"))
        self.assertIn("non-mutating", str(ctx.exception))

    def test_mutation_that_breaks_imports_fails_validation(self):
        """The import check is a real oracle: break a path, validation must catch it."""
        tree = os.path.join(self.out("s7"), "tree")
        self.forge(seed=7)
        os.rename(
            os.path.join(tree, "beta", "models.py"),
            os.path.join(tree, "beta", "models_moved.py"),
        )
        with open(os.path.join(tree, "beta", "client.py"), "a") as f:
            f.write("\nfrom beta.models import Record\n")

        result = forge.run_validation(
            {"argv": ["{python}", "{forge}", "check-imports", "--language", "python"]},
            tree,
        )
        self.assertFalse(result["passed"])


class TestRealCorporaConfig(unittest.TestCase):
    """The shipped config is data the forge depends on; assert it, do not run it."""

    def test_corpora_json_declares_the_three_pinned_sources(self):
        specs = forge.load_corpora(forge.CORPORA_PATH)
        self.assertEqual(
            sorted(specs), ["next.js", "pocketbase", "scikit-learn"]
        )
        for spec in specs.values():
            self.assertEqual(len(spec["pinned_commit"]), 40)
            self.assertTrue(spec["validation"]["argv"])
            self.assertTrue(spec["mutations"]["decoys"] >= 1)
            self.assertTrue(os.path.isabs(spec["source_path"]))


if __name__ == "__main__":
    unittest.main()
