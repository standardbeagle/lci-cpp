"""Conformance-harness adapter for the recursive tagged-block candidate."""

import importlib.util
from pathlib import Path

_SOURCE = Path(__file__).with_name("semi_structured.py")
_SPEC = importlib.util.spec_from_file_location("lci_semi_structured_tagged_blocks", _SOURCE)
if _SPEC is None or _SPEC.loader is None:
    raise RuntimeError(f"cannot load candidate implementation: {_SOURCE}")
_CODEC = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(_CODEC)

NAME = "tagged-blocks-v1"
encode = _CODEC.encode_tagged_blocks
decode = _CODEC.decode_tagged_blocks
