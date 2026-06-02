package api

// Route dispatches a method+path to a handler. Impure (calls I/O handlers).
func Route(method, path, body string) int {
	switch method {
	case "GET":
		return HandleGet(path)
	case "POST":
		return HandlePost(path, body)
	default:
		Logf("unsupported method %s", method)
		return 405
	}
}

// RouteAll dispatches a batch of requests and tallies statuses.
func RouteAll(reqs [][3]string) int {
	ok := 0
	for _, r := range reqs {
		if Route(r[0], r[1], r[2]) < 400 {
			ok++
		}
	}
	return ok
}
