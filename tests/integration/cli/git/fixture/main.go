// Package main is a minimal committed fixture used by the git-analyze
// integration spec. The integration runner copies this directory into a fresh
// temp git repo and commits it, so `git-analyze --scope wip` sees a clean
// working tree (no uncommitted changes) deterministically. Contents are
// incidental — the spec only asserts the "no changes to analyze" summary.
package main

import "fmt"

func main() {
	fmt.Println(greeting("world"))
}

func greeting(name string) string {
	return "hello, " + name
}
