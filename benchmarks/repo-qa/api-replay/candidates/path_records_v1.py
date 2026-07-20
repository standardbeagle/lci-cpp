"""Conformance-harness adapter for the framed typed-path candidate."""

import importlib.util
from pathlib import Path

_SOURCE = Path(__file__).with_name("semi_structured.py")
_SPEC = importlib.util.spec_from_file_location("lci_semi_structured_path_records", _SOURCE)
if _SPEC is None or _SPEC.loader is None:
    raise RuntimeError(f"cannot load candidate implementation: {_SOURCE}")
_CODEC = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(_CODEC)

NAME = "path-records-v1"
encode = _CODEC.encode_path_records
decode = _CODEC.decode_path_records
