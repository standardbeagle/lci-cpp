"""Formatting helpers. Pure functions for the utils module."""


def pad_left(s, width, fill=" "):
    """Left-pad s to width with fill. Pure."""
    if len(s) >= width:
        return s
    return fill * (width - len(s)) + s


def join_lines(lines):
    """Join lines with newlines. Pure."""
    return "\n".join(lines)


def truncate(s, limit):
    """Truncate s to limit characters with an ellipsis. Pure."""
    if len(s) <= limit:
        return s
    return s[: limit - 1] + "…"
