# Captured-request format exploration

This directory is reserved for the exploratory four-arm replay of production
JSON, readable XML, framed path records, and tagged blocks. The runner uses opaque
arm labels and accepts only per-model fixtures captured from that same model. It
consumes the frozen manifest and eight-block Latin-square schedule rather than
generating an order at run time.

Running `api_replay_format_exploration.py` validates and prints a deterministic
plan. It cannot contact a provider. Provider execution requires an injected
implementation and the explicit in-process `allow_provider=True` gate. Result
records are created exclusively and are never overwritten.

The adjacent manifest and schedule are the exploratory preregistration. This
directory contains genuine provider-native DeepSeek and GLM captures of the same
prompt, one-call LCI interaction, tool-result value, corpus revision, and expected
answer. The runner never relabels or reuses one provider's request for another.

No replay outcomes have been collected. Do not interpret codec conformance, model
interviews, capture answers, or request construction as comprehension evidence.
