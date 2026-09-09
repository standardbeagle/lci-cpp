package main

// ComputeTotal and ComputeTotalTwo are deliberately near-identical (same
// body, different name) so the git-analyze integration specs pin a REAL
// structural-duplicate finding instead of an envelope-only empty report.
func ComputeTotal(a int, b int) int {
	sum := a + b
	sum = sum * 2
	if sum > 100 {
		sum = 100
	}
	return sum
}

func ComputeTotalTwo(a int, b int) int {
	sum := a + b
	sum = sum * 2
	if sum > 100 {
		sum = 100
	}
	return sum
}
