package api

// Legacy helpers using non-obvious / PHP-style names — exercises the
// VOCABULARY naming signal.

// explode splits s on sep (PHP's name for string split).
func explode(s, sep string) []string {
	var out []string
	cur := ""
	for _, r := range s {
		if string(r) == sep {
			out = append(out, cur)
			cur = ""
		} else {
			cur += string(r)
		}
	}
	return append(out, cur)
}

// implode joins parts with sep (PHP's name for string join).
func implode(parts []string, sep string) string {
	out := ""
	for i, p := range parts {
		if i > 0 {
			out += sep
		}
		out += p
	}
	return out
}

// frobnicate is deliberately jargon — no standard synonym, hard to search for.
func frobnicate(x int) int {
	return x*3 + 1
}

func legacyDemo() {
	explode("a/b/c", "/")
	implode([]string{"a", "b"}, "/")
	frobnicate(2)
}

func legacyDemoTwo() {
	explode("x,y", ",")
	frobnicate(9)
}
