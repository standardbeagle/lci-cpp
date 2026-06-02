package core

import "strings"

// Kind enumerates token classes for the validator.
type Kind int

const (
	KindUnknown Kind = iota
	KindNumber
	KindWord
	KindSymbol
)

// Classify is intentionally branch-heavy to exercise complexity metrics.
func Classify(s string) Kind {
	if s == "" {
		return KindUnknown
	}
	if s == "+" || s == "-" || s == "*" || s == "/" {
		return KindSymbol
	}
	allDigit := true
	allAlpha := true
	for _, r := range s {
		if r < '0' || r > '9' {
			allDigit = false
		}
		if !(r >= 'a' && r <= 'z') && !(r >= 'A' && r <= 'Z') {
			allAlpha = false
		}
	}
	if allDigit {
		return KindNumber
	}
	if allAlpha {
		return KindWord
	}
	return KindUnknown
}

// ValidateName runs a long chain of checks (long-function + branchy).
func ValidateName(name string) (bool, string) {
	if name == "" {
		return false, "empty"
	}
	if len(name) < 2 {
		return false, "too short"
	}
	if len(name) > 64 {
		return false, "too long"
	}
	if strings.HasPrefix(name, "_") {
		return false, "leading underscore"
	}
	if strings.HasSuffix(name, "_") {
		return false, "trailing underscore"
	}
	if strings.Contains(name, " ") {
		return false, "contains space"
	}
	if strings.Contains(name, "..") {
		return false, "double dot"
	}
	first := rune(name[0])
	if first >= '0' && first <= '9' {
		return false, "leading digit"
	}
	for _, r := range name {
		switch {
		case r >= 'a' && r <= 'z':
		case r >= 'A' && r <= 'Z':
		case r >= '0' && r <= '9':
		case r == '_' || r == '-' || r == '.':
		default:
			return false, "illegal char"
		}
	}
	return true, "ok"
}
