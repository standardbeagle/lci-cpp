// Command pb-api-2.probe is the discrimination probe for edit task pb-api-2
// (go-request-event-handler-shape): the route handler recordUpdate in
// apis/record_crud.go must have the uniform handler shape its registered
// peers use - exactly one *core.RequestEvent parameter and a single error
// result - so it registers on the router without a factory adapter.
//
// The probe is a go/parser AST assertion over the materialized tree (cwd).
// Exit 0: the handler has the uniform shape. Exit 1: otherwise.
package main

import (
	"fmt"
	"go/ast"
	"go/parser"
	"go/token"
	"os"
)

const targetFile = "apis/record_crud.go"
const targetFunc = "recordUpdate"

func main() {
	fset := token.NewFileSet()
	f, err := parser.ParseFile(fset, targetFile, nil, 0)
	if err != nil {
		fmt.Fprintln(os.Stderr, "probe: parse:", err)
		os.Exit(1)
	}
	for _, decl := range f.Decls {
		fn, ok := decl.(*ast.FuncDecl)
		if !ok || fn.Name.Name != targetFunc || fn.Recv != nil {
			continue
		}
		if isHandlerShape(fn.Type) {
			os.Exit(0)
		}
		fmt.Fprintf(os.Stderr,
			"probe: %s in %s departs from the uniform "+
				"func(*core.RequestEvent) error handler shape\n",
			targetFunc, targetFile)
		os.Exit(1)
	}
	fmt.Fprintf(os.Stderr, "probe: handler %s not found in %s\n", targetFunc, targetFile)
	os.Exit(1)
}

// isHandlerShape reports whether t is func(*core.RequestEvent) error.
func isHandlerShape(t *ast.FuncType) bool {
	if t.Params == nil || len(t.Params.List) != 1 {
		return false
	}
	star, ok := t.Params.List[0].Type.(*ast.StarExpr)
	if !ok {
		return false
	}
	sel, ok := star.X.(*ast.SelectorExpr)
	if !ok || sel.Sel.Name != "RequestEvent" {
		return false
	}
	pkg, ok := sel.X.(*ast.Ident)
	if !ok || pkg.Name != "core" {
		return false
	}
	if t.Results == nil || len(t.Results.List) != 1 {
		return false
	}
	id, ok := t.Results.List[0].Type.(*ast.Ident)
	return ok && id.Name == "error"
}
