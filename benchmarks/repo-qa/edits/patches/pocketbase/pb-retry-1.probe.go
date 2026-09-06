// Command pb-retry-1.probe is the discrimination probe for edit task
// pb-retry-1 (go-wrap-errors-percent-w): the filepath.Abs failure return in
// New of tools/filesystem/internal/fileblob/fileblob.go must wrap the
// underlying cause with fmt.Errorf and the %w verb so errors.Is/As can reach
// it - a %v rendering drops the cause chain.
//
// The probe is a go/parser AST assertion over the materialized tree (cwd).
// Exit 0: the matching fmt.Errorf call uses %w. Exit 1: otherwise.
package main

import (
	"fmt"
	"go/ast"
	"go/parser"
	"go/token"
	"os"
	"strconv"
	"strings"
)

const targetFile = "tools/filesystem/internal/fileblob/fileblob.go"
const targetFunc = "New"

// formatNeedle selects the one fmt.Errorf call site under test.
const formatNeedle = "absolute path"

func main() {
	fset := token.NewFileSet()
	f, err := parser.ParseFile(fset, targetFile, nil, 0)
	if err != nil {
		fmt.Fprintln(os.Stderr, "probe: parse:", err)
		os.Exit(1)
	}
	for _, decl := range f.Decls {
		fn, ok := decl.(*ast.FuncDecl)
		if !ok || fn.Name.Name != targetFunc {
			continue
		}
		found, wrapped := false, false
		ast.Inspect(fn.Body, func(n ast.Node) bool {
			call, ok := n.(*ast.CallExpr)
			if !ok || !isFmtErrorf(call) || len(call.Args) == 0 {
				return true
			}
			lit, ok := call.Args[0].(*ast.BasicLit)
			if !ok || lit.Kind != token.STRING {
				return true
			}
			format, err := strconv.Unquote(lit.Value)
			if err != nil || !strings.Contains(format, formatNeedle) {
				return true
			}
			found = true
			wrapped = strings.Contains(format, "%w")
			return true
		})
		if !found {
			fmt.Fprintf(os.Stderr,
				"probe: no fmt.Errorf call matching %q in %s (%s)\n",
				formatNeedle, targetFunc, targetFile)
			os.Exit(1)
		}
		if !wrapped {
			fmt.Fprintf(os.Stderr,
				"probe: %s in %s reports the %s failure without %%w; "+
					"the cause chain is lost\n",
				targetFunc, targetFile, formatNeedle)
			os.Exit(1)
		}
		os.Exit(0)
	}
	fmt.Fprintf(os.Stderr, "probe: func %s not found in %s\n", targetFunc, targetFile)
	os.Exit(1)
}

func isFmtErrorf(call *ast.CallExpr) bool {
	sel, ok := call.Fun.(*ast.SelectorExpr)
	if !ok || sel.Sel.Name != "Errorf" {
		return false
	}
	pkg, ok := sel.X.(*ast.Ident)
	return ok && pkg.Name == "fmt"
}
