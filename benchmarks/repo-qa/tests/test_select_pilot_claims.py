"""The pilot subset must be derived, not chosen.

A hand-picked subset lets whoever picks it pick the result, so these pin that
the selection rule is deterministic, balanced, and reproduces the committed
list. The discrimination case is the one that matters: a bank the balance rule
cannot satisfy must RAISE, not silently return an unbalanced subset.
"""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH_ROOT = os.path.dirname(HERE)
SCRIPTS = os.path.join(BENCH_ROOT, "scripts")
if SCRIPTS not in sys.path:
    sys.path.insert(0, SCRIPTS)

import select_pilot_claims as sel  # noqa: E402

# The committed pilot subset. A change to the bank, the seed or the rule must
# fail here rather than quietly re-draw the experiment.
EXPECTED = [
    ("dead-code", "scikit-learn", "skl-estimator-copy"),
    ("false-premise", "pocketbase", "pb-signed-token-mint-verify"),
    ("misleading-doc", "pocketbase", "pb-realtime-fanout-hub"),
    ("true", "next.js", "nx-redirect-status-codes"),
    ("unsupported", "next.js", "nx-rewrite-claim-boundary"),
    ("wrong-layer", "scikit-learn", "skl-conditional-method"),
]


class SelectionTest(unittest.TestCase):
    def setUp(self):
        self.bank = sel.load_bank()

    def test_reproduces_the_committed_subset(self):
        picked = [(r["category"], r["corpus"], r["id"]) for r in sel.select(self.bank)]
        self.assertEqual(picked, EXPECTED)

    def test_is_deterministic_across_calls(self):
        self.assertEqual(sel.select(self.bank), sel.select(self.bank))

    def test_covers_every_category_exactly_once(self):
        picked = sel.select(self.bank)
        categories = [r["category"] for r in picked]
        self.assertEqual(sorted(categories), sorted(set(categories)))
        self.assertEqual(set(categories), {r["category"] for r in self.bank})

    def test_is_balanced_two_per_corpus(self):
        picked = sel.select(self.bank)
        counts = {}
        for row in picked:
            counts[row["corpus"]] = counts.get(row["corpus"], 0) + 1
        self.assertEqual(set(counts.values()), {sel.PER_CORPUS})

    def test_a_different_seed_draws_a_different_subset(self):
        # If it did not, the seed would be decorative and the "derived, not
        # chosen" claim would rest on nothing.
        other = sel.select(self.bank, seed=11)
        self.assertNotEqual([r["id"] for r in other],
                            [r["id"] for r in sel.select(self.bank)])

    def test_an_unsatisfiable_balance_raises_instead_of_returning_a_skew(self):
        single_corpus = [dict(row, corpus="only") for row in self.bank]
        with self.assertRaises(ValueError):
            sel.select(single_corpus)


if __name__ == "__main__":
    unittest.main()
