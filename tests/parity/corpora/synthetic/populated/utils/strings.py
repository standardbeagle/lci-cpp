"""String helpers. Pure functions for the utils module."""


def reverse(s):
    """Return the reversed string. Pure."""
    return s[::-1]


def is_palindrome(s):
    """Report whether s reads the same forwards and backwards. Pure."""
    cleaned = "".join(c.lower() for c in s if c.isalnum())
    return cleaned == cleaned[::-1]


def title_case(s):
    """Capitalize the first letter of each whitespace-delimited word. Pure."""
    out = []
    for word in s.split(" "):
        if not word:
            out.append(word)
        else:
            out.append(word[0].upper() + word[1:])
    return " ".join(out)


def count_vowels(s):
    """Count vowels in s. Pure."""
    total = 0
    for c in s.lower():
        if c in "aeiou":
            total += 1
    return total
