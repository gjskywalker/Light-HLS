#!/usr/bin/env python3
"""Keep observed done, final commit, and acceptance interval distinct."""
import unittest
from run_u250_fair_comparison import ComparisonError, parse_transaction_events


class TransactionEvents(unittest.TestCase):
    def test_early_done_is_not_final_commit(self):
        parsed = parse_transaction_events([
            "EVENT trial=0 accepted=10 first_read=11 first_write=12 last_commit=29 done=28",
            "EVENT trial=1 accepted=35 first_read=36 first_write=37 last_commit=54 done=53",
        ])
        self.assertEqual(parsed["final_commit_cycles"], [19, 19])
        self.assertEqual(parsed["completion_signal_cycles"], [18, 18])
        self.assertEqual(parsed["observed_acceptance_intervals"], [25])

    def test_missing_events_are_not_invented(self):
        self.assertEqual(parse_transaction_events(["TESTS PASSED: cycles=5"])["transaction_events"], [])

    def test_invalid_trace(self):
        for line in ("EVENT trial=0 accepted=4", "EVENT trial=0 accepted=4 last_commit=3 done=6",
                     "EVENT trial=0 accepted=4 last_commit=5 done=-1"):
            with self.assertRaises(ComparisonError):
                parse_transaction_events([line])


if __name__ == "__main__":
    unittest.main()
