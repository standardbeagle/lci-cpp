"""Parse file-and-line citations out of an agent's free-text final answer.

The agent explored a MUTATED corpus checkout, so any `path:line` it cites is
already against the mutated tree -- the same coordinate space the adjudicated
answer key uses. Parsing therefore normalises coordinates, it does not translate
them: a citation becomes a `(path, start, end)` anchor with a leading `./`
stripped and back-slashes folded to `/`, so `./a\\b.ts:3` and `a/b.ts:3` collapse
to one anchor. The result is sorted and de-duplicated so a duplicated citation
can never inflate a downstream score.
"""

import re

# path (must carry a file extension) then a line coordinate:
#   file.ts:3        -> (3, 3)
#   file.ts:5-9      -> (5, 9)   (en-dash accepted too)
#   file.ts:L7       -> (7, 7)   (leading L on either bound tolerated)
# A trailing :col (file.ts:3:10) is ignored -- only the line is captured.
_CITATION = re.compile(
    r"(?P<path>[A-Za-z0-9_./\\-]+\.[A-Za-z0-9]+)"
    r"[:#]L?(?P<start>\d+)"
    r"(?:\s*[-–]\s*L?(?P<end>\d+))?"
)


def normalize_path(path):
    """Fold separators and strip leading `./` so equal files compare equal."""
    normalized = path.replace("\\", "/")
    while normalized.startswith("./"):
        normalized = normalized[2:]
    return normalized


def parse_citations(text):
    """Return sorted, de-duplicated `(path, start, end)` anchors cited in text.

    `end` is always >= `start`; an inverted `9-5` is normalised to `5-9`.
    """
    if not text:
        return []
    found = set()
    for match in _CITATION.finditer(text):
        path = normalize_path(match.group("path"))
        start = int(match.group("start"))
        end = int(match.group("end")) if match.group("end") else start
        if end < start:
            start, end = end, start
        found.add((path, start, end))
    return sorted(found)
