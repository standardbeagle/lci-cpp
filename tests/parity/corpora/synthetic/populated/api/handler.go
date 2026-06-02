package api

import (
	"fmt"
	"os"
)

// Logf writes a formatted line to stderr. Impure (I/O).
func Logf(format string, args ...interface{}) {
	fmt.Fprintf(os.Stderr, format+"\n", args...)
}

// HandleGet prints a response and returns a status code. Impure (I/O).
func HandleGet(path string) int {
	if path == "" {
		Logf("empty path")
		return 400
	}
	fmt.Println("GET", path)
	return 200
}

// HandlePost validates and echoes a body. Impure (I/O).
func HandlePost(path, body string) int {
	if body == "" {
		Logf("empty body for %s", path)
		return 422
	}
	fmt.Println("POST", path, body)
	return 201
}
