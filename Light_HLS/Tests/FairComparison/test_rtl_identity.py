#!/usr/bin/env python3

"""Check the deliberately narrow normalization used by the RTL identity audit."""

import unittest

from verify_archived_rtl import canonicalize_zero_assignments as normalize


class ZeroAssignmentTests(unittest.TestCase):
    def test_independent_zero_assignments(self):
        self.assertEqual(normalize("  b <= 3'd0;\n  a <= 8'd0;\n"),
                         "  a <= 8'd0;\n  b <= 3'd0;\n")

    def test_value_dependencies_are_not_reordered(self):
        original = "  b <= a;\n  a <= 8'd0;\n"
        self.assertEqual(normalize(original), original)

    def test_duplicate_destinations_are_not_reordered(self):
        original = "  b <= 3'd0;\n  a <= 8'd0;\n  b <= 4'd0;\n"
        self.assertEqual(normalize(original), original)

    def test_control_boundaries_are_preserved(self):
        original = "  b <= 3'd0;\n  if (enable) begin\n    a <= 8'd0;\n  end\n"
        self.assertEqual(normalize(original), original)

    def test_nonzero_assignments_are_not_reordered(self):
        original = "  b <= 3'd1;\n  a <= 8'd0;\n"
        self.assertEqual(normalize(original), original)


if __name__ == "__main__":
    unittest.main()
