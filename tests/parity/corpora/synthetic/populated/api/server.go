package api

import "fmt"

// Serve runs a fixed batch of requests and reports how many succeeded. Impure.
func Serve(reqs [][3]string) {
	ok := RouteAll(reqs)
	fmt.Printf("served %d/%d ok\n", ok, len(reqs))
}
