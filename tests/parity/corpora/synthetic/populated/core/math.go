package core

// Add returns the sum of two integers. Pure.
func Add(a, b int) int {
	return a + b
}

// Mul returns the product of two integers. Pure.
func Mul(a, b int) int {
	return a * b
}

// Clamp restricts v to the inclusive range [lo, hi]. Pure.
func Clamp(v, lo, hi int) int {
	if v < lo {
		return lo
	}
	if v > hi {
		return hi
	}
	return v
}

// Sum folds a slice into its total. Pure.
func Sum(xs []int) int {
	total := 0
	for _, x := range xs {
		total += x
	}
	return total
}
