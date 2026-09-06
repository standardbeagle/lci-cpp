// Command pb-log-2.probe is the discrimination probe for edit task pb-log-2
// (go-structured-logger-attrs): the "invalid AllowOrigins pattern" diagnostic
// inside CORS in apis/middlewares_cors.go must go through slog with a message
// plus key/value attribute pairs - never fmt.Print*/log.Print*.
//
// The probe is a go/parser AST assertion over the materialized tree (cwd).
// Exit 0: the diagnostic is a structured slog/app-logger call with at least
// one attribute argument. Exit 1: otherwise.
package main

import (
	"fmt"
	"go/ast"
	"go/parser"
	"go/token"
	"os"
)

const targetFile = "apis/middlewares_cors.go"
const targetFunc = "CORS"

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
				"probe: %s in %s emits a diagnostic via %s instead of a "+
					"structured slog call\n",
				targetFunc, targetFile, v.adhoc)
			os.Exit(1)
		}
		if v.structured {
			os.Exit(0)
		}
	}
	fmt.Fprintf(os.Stderr,
		"probe: no structured diagnostic found in %s (%s)\n",
		targetFunc, targetFile)
	os.Exit(1)
}

// diagnosticVisitor records ad-hoc print calls and structured logger calls.
type diagnosticVisitor struct {
	adhoc      string
	structured bool
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
		// A structured diagnostic carries a message plus at least one
		// attribute argument; its receiver is the slog package or an
		// app .Logger() call.
		if len(call.Args) >= 2 && isStructuredLogger(sel.X) {
			v.structured = true
		}
	}
	return v
}

func isStructuredLogger(x ast.Expr) bool {
	if pkg, ok := x.(*ast.Ident); ok && pkg.Name == "slog" {
		return true
	}
	inner, ok := x.(*ast.CallExpr)
	if !ok {
		return false
	}
	sel, ok := inner.Fun.(*ast.SelectorExpr)
	return ok && sel.Sel.Name == "Logger"
}
