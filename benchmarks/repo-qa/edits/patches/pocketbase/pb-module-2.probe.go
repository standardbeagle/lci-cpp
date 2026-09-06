// Command pb-module-2.probe is the discrimination probe for edit task
// pb-module-2 (go-constructor-new-pointer): the SendEvent type in
// tools/mailer/mailer.go must expose a NewSendEvent() *SendEvent constructor
// like its sibling tool types, and the callers in sendmail.go and smtp.go
// must obtain the value through it instead of hand-assembling a &SendEvent{}
// composite literal.
//
// The probe is a go/parser AST assertion over the materialized tree (cwd).
// Exit 0: constructor exists and both callers use it. Exit 1: otherwise.
package main

import (
	"fmt"
	"go/ast"
	"go/parser"
	"go/token"
	"os"
)

func main() {
	if !hasPointerConstructor("tools/mailer/mailer.go", "NewSendEvent", "SendEvent") {
		fmt.Fprintln(os.Stderr,
			"probe: tools/mailer/mailer.go has no NewSendEvent() *SendEvent constructor")
		os.Exit(1)
	}
	for _, caller := range []string{
		"tools/mailer/sendmail.go",
		"tools/mailer/smtp.go",
	} {
		if hasCompositeLiteral(caller, "SendEvent") {
			fmt.Fprintf(os.Stderr,
				"probe: %s still assembles &SendEvent{} instead of calling NewSendEvent\n",
				caller)
			os.Exit(1)
		}
	}
	os.Exit(0)
}

// hasPointerConstructor reports whether file declares
// func <name>() *<typ>.
func hasPointerConstructor(file, name, typ string) bool {
	fset := token.NewFileSet()
	f, err := parser.ParseFile(fset, file, nil, 0)
	if err != nil {
		fmt.Fprintln(os.Stderr, "probe: parse:", err)
		os.Exit(1)
	}
	for _, decl := range f.Decls {
		fn, ok := decl.(*ast.FuncDecl)
		if !ok || fn.Name.Name != name || fn.Recv != nil {
			continue
		}
		if fn.Type.Results == nil || len(fn.Type.Results.List) != 1 {
			continue
		}
		star, ok := fn.Type.Results.List[0].Type.(*ast.StarExpr)
		if !ok {
			continue
		}
		if id, ok := star.X.(*ast.Ident); ok && id.Name == typ {
			return true
		}
	}
	return false
}

// hasCompositeLiteral reports whether file contains a &<typ>{...} composite
// literal expression.
func hasCompositeLiteral(file, typ string) bool {
	fset := token.NewFileSet()
	f, err := parser.ParseFile(fset, file, nil, 0)
	if err != nil {
		fmt.Fprintln(os.Stderr, "probe: parse:", err)
		os.Exit(1)
	}
	found := false
	ast.Inspect(f, func(n ast.Node) bool {
		u, ok := n.(*ast.UnaryExpr)
		if !ok || u.Op != token.AND {
			return true
		}
		lit, ok := u.X.(*ast.CompositeLit)
		if !ok {
			return true
		}
		if id, ok := lit.Type.(*ast.Ident); ok && id.Name == typ {
			found = true
		}
		return true
	})
	return found
}
