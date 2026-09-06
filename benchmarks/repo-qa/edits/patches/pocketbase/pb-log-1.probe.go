// Command pb-log-1.probe is the discrimination probe for edit task pb-log-1
// (go-structured-logger-attrs): the "Failed to write log" diagnostic inside
// (*BaseApp).initLogger in core/base.go must go through the app logger with a
// message plus key/value attribute pairs - never fmt.Print*/log.Print*.
//
// The probe is a go/parser AST assertion over the materialized tree (cwd).
// Exit 0: the diagnostic is a structured logger call with at least one
// attribute argument. Exit 1: otherwise.
package main

import (
	"fmt"
	"go/ast"
	"go/parser"
	"go/token"
	"os"
)

const targetFile = "core/base.go"
const targetFunc = "initLogger"

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
		v := &diagnosticVisitor{}
		ast.Walk(v, fn.Body)
		if v.adhoc != "" {
			fmt.Fprintf(os.Stderr,
				"probe: %s in %s emits a diagnostic via %s instead of the "+
					"structured app logger\n",
				targetFunc, targetFile, v.adhoc)
			os.Exit(1)
		}
		if v.structured {
			os.Exit(0)
		}
	}
	fmt.Fprintf(os.Stderr,
		"probe: no structured logger diagnostic found in %s (%s)\n",
		targetFunc, targetFile)
	os.Exit(1)
}

// diagnosticVisitor records ad-hoc print calls and structured logger calls.
type diagnosticVisitor struct {
	adhoc      string // description of the first fmt/log.Print* call
	structured bool   // saw a .Logger()/slog level call with attr args
}

func (v *diagnosticVisitor) Visit(n ast.Node) ast.Visitor {
	call, ok := n.(*ast.CallExpr)
	if !ok {
		return v
	}
	sel, ok := call.Fun.(*ast.SelectorExpr)
	if !ok {
		return v
	}
	switch sel.Sel.Name {
	case "Print", "Printf", "Println":
		if pkg, ok := sel.X.(*ast.Ident); ok && (pkg.Name == "fmt" || pkg.Name == "log") {
			if v.adhoc == "" {
				v.adhoc = pkg.Name + "." + sel.Sel.Name
			}
		}
	case "Debug", "Info", "Warn", "Error":
		// The structured diagnostic carries a message plus at least one
		// attribute argument, and its receiver is an app .Logger() call.
		if len(call.Args) >= 2 && isLoggerCall(sel.X) {
			v.structured = true
		}
	}
	return v
}

// isLoggerCall reports whether x is a call expression selecting Logger
// (e.g. txApp.Logger()).
func isLoggerCall(x ast.Expr) bool {
	inner, ok := x.(*ast.CallExpr)
	if !ok {
		return false
	}
	sel, ok := inner.Fun.(*ast.SelectorExpr)
	return ok && sel.Sel.Name == "Logger"
}
