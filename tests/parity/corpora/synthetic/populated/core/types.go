package core

// Pair groups two integers.
type Pair struct {
	First  int
	Second int
}

// Swap returns the pair with its fields exchanged. Pure.
func (p Pair) Swap() Pair {
	return Pair{First: p.Second, Second: p.First}
}

// Total returns the sum of both fields. Pure.
func (p Pair) Total() int {
	return p.First + p.Second
}
