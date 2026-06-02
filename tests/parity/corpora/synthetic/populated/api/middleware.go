package api

import "fmt"

// LogMiddleware wraps a status with a logged line. Impure (I/O).
func LogMiddleware(method, path string, status int) int {
	fmt.Println("mw", method, path, status)
	return status
}

// AuthMiddleware rejects requests without a token. Pure-ish (no I/O).
func AuthMiddleware(token string, status int) int {
	if token == "" {
		return 401
	}
	return status
}
