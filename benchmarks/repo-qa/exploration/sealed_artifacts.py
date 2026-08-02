"""The explicit inventory of harness-owned sealed artefacts.

Sealed material -- author annotations, adjudicated task banks, answer keys --
lives outside the agent's checkout, so the primary defence is the gate's
path-escape check. This module is the second line: it names the locations the
harness itself owns, so an artefact accidentally copied *into* a checkout is
still refused.

Identification is by LOCATION, never by words in a filename. A word heuristic
cannot distinguish `annotations/key.json` from a corpus file legitimately
called `load-manifest.external.ts`, `tracer.ts` or `oracle_client.py`, and
guessing wrong is not a soft failure: reading such a file is recorded as a tool
violation and citing it as a malformed answer, both terminal. Corpus content is
arbitrary third-party source and may contain any word; only these locations are
ours to seal.

Adding a sealed artefact means adding its location here.
"""

# Directories the harness owns, as checkout-relative prefixes. Both the
# in-repo locations (a checkout that accidentally contains the benchmark tree)
# and the root-anchored copies a mis-staged run would produce are listed; a
# corpus's own deep `docs/annotations/` is deliberately NOT sealed.
SEALED_LOCATIONS = (
    "annotations",
    "answer-keys",
    "answer_keys",
    "benchmarks/repo-qa/annotations",
    "benchmarks/repo-qa/exploration/annotations",
    "benchmarks/repo-qa/exploration/tasks",
    "benchmarks/repo-qa/exploration/schema",
    # The gate also serves the EDIT-mode arms (edits/runner): the edit bank's
    # tasks, dual annotations, schema, and oracle harness are answer-key
    # material of exactly the same class. (The edits tree has no goldens dir.)
    "benchmarks/repo-qa/edits/tasks",
    "benchmarks/repo-qa/edits/annotations",
    "benchmarks/repo-qa/edits/schema",
    "benchmarks/repo-qa/edits/oracles",
)


def normalize(candidate):
    """Checkout-relative, forward-slashed, without `./` segments."""
    parts = [part for part in candidate.replace("\\", "/").split("/")
             if part not in ("", ".")]
    return "/".join(parts)


def is_sealed_path(candidate):
    """True when `candidate` names a harness-owned sealed location or something
    inside one. Case-sensitive: these are paths the harness writes itself."""
    normal = normalize(candidate)
    return any(normal == location or normal.startswith(location + "/")
               for location in SEALED_LOCATIONS)
