"""Score one arm's free-text answer against an oracle answer SET.

Precision AND recall, never a single-string match: an arm that names 3 of 12
real call sites has perfect precision and is still wrong, and only a set-wise
comparison can say so. `correct` is reserved for precision == recall == 1.0, so
no partial answer can be reported as a correct one.

Only CITED locations count. A claim with no `path:line` behind it earns no
credit, because a benchmark that rewards confident prose measures fluency.
Citation parsing is reused from the exploration bank
(`exploration/scoring/citations.py`) rather than re-implemented: a second
regex over the same syntax would be a second blind spot to maintain
(`.claude/rules/bench-harness-oracle-independence.md` rule 1 -- the shared
mechanism here is the ANSWER parser, which both arms feed equally, not the
oracle, which shares nothing with either arm).
"""

import os
import sys

_BENCH_ROOT = os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
)
if os.path.join(_BENCH_ROOT, "exploration") not in sys.path:
    sys.path.insert(0, os.path.join(_BENCH_ROOT, "exploration"))

from scoring.citations import normalize_path, parse_citations  # noqa: E402

# Why a cell carries no gradable score. Recorded on the row so a reader can
# tell "the oracle set was empty" from "the arm scored zero" -- collapsing
# those two is how an unanswerable cell becomes a fake loss.
REASON_EMPTY_ANSWER_SET = "EMPTY_ANSWER_SET"
REASON_NOT_ANSWER_SET_GRADED = "NOT_ANSWER_SET_GRADED"


def parse_answer_locations(text):
    """Distinct `(path, line)` locations cited in an arm's answer.

    A cited range collapses to its start line: the oracle's answer sets are
    point locations, so a range that begins at the right line is a hit.
    """
    return sorted({(path, start) for path, start, _end in parse_citations(text)})


def _matches(cited, truth_location):
    """Path-suffix tolerant equality on (path, line).

    An arm may cite `pocketbase/apis/record.go:12` or `apis/record.go:12` for
    the same location depending on how it names the checkout root; the line
    number must match exactly.
    """
    cited_path, cited_line = cited
    truth_path, truth_line = normalize_path(truth_location[0]), truth_location[1]
    if cited_line != truth_line:
        return False
    return cited_path == truth_path or cited_path.endswith("/" + truth_path)


def score_answer(text, truth):
    """Grade `text` against an answer set built by `answer_sets`.

    Returns a score block. When the answer set is not gradable (an empty oracle
    set, or a family graded on completion rather than content) every metric is
    None and `ungradable_reason` says which -- never 0.0, which would read as a
    measured loss.
    """
    cited = parse_answer_locations(text)
    if not truth.get("gradable"):
        return {
            "precision": None,
            "recall": None,
            "f1": None,
            "correct": None,
            "ungradable_reason": truth.get("ungradable_reason", REASON_EMPTY_ANSWER_SET),
            "reported_empty": not cited,
            "cited_total": len(cited),
            "answer_set_total": len(truth.get("locations", [])),
        }

    locations = truth["locations"]
    matched = set()
    valid = 0
    for anchor in cited:
        hit = False
        for index, location in enumerate(locations):
            if _matches(anchor, location):
                matched.add(index)
                hit = True
        if hit:
            valid += 1

    precision = valid / len(cited) if cited else 0.0
    recall = len(matched) / len(locations)
    f1 = (2 * precision * recall / (precision + recall)) if (precision + recall) else 0.0
    return {
        "precision": precision,
        "recall": recall,
        "f1": f1,
        "correct": precision == 1.0 and recall == 1.0,
        "ungradable_reason": None,
        "reported_empty": not cited,
        "cited_total": len(cited),
        "cited_valid": valid,
        "cited_invalid": len(cited) - valid,
        "answer_set_total": len(locations),
        "answer_set_matched": len(matched),
    }


def null_score(reason):
    """Score block for a cell with no answer to grade (a DNF)."""
    return {
        "precision": None,
        "recall": None,
        "f1": None,
        "correct": None,
        "ungradable_reason": reason,
        "reported_empty": False,
        "cited_total": 0,
        "answer_set_total": None,
    }
