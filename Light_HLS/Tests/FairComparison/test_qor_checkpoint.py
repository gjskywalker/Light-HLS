#!/usr/bin/env python3

"""Check that checkpoint audits expose regressions and reject incomparable data."""

import copy
import unittest

from compare_u250_baselines import compare


def fixture():
    qor = {"tool_version": "2025.2", "flow_id": "normalized",
           "resources": {"lut": 20, "ff": 30, "dsp": 0, "bram_tiles": 0},
           "timing": {"clock_constrained": True, "worst_slack_ns": 2}}
    case = {"suite": "pipeline", "case": "example", "source": "example.cpp", "top": "example",
            "source_sha256": "source", "testbench_sha256": "testbench", "config_sha256": None,
            "valid": True, "reward_eligible": True, "functional_equivalence": True,
            "normalized_interface_equivalence": True, "simulation_cycles": {"light_hls": 10},
            "light_hls": copy.deepcopy(qor), "vitis_hls": copy.deepcopy(qor)}
    return {"valid": True, "reward_eligible": True, "board": "u250", "part": "u250-part",
            "flow": "normalized", "clock_period_ns": [10], "cases": [case]}


class CheckpointTest(unittest.TestCase):
    def test_unchanged_and_improved(self):
        before, after = fixture(), fixture()
        self.assertEqual(compare(before, after)["counts"], {"unchanged": 1})
        after["cases"][0]["light_hls"]["resources"]["lut"] = 10
        self.assertTrue(compare(before, after)["no_metric_regressions"])
        self.assertEqual(compare(before, after)["counts"], {"improved": 1})

    def test_tradeoff_is_not_hidden(self):
        before, after = fixture(), fixture()
        after["cases"][0]["simulation_cycles"]["light_hls"] = 8
        after["cases"][0]["light_hls"]["resources"]["ff"] = 31
        result = compare(before, after)
        self.assertEqual(result["counts"], {"tradeoff": 1})
        self.assertFalse(result["no_metric_regressions"])

    def test_missing_duplicate_and_changed_cases(self):
        for change in (lambda data: data["cases"].clear(),
                       lambda data: data["cases"].append(copy.deepcopy(data["cases"][0])),
                       lambda data: data["cases"][0].update(source_sha256="changed"),
                       lambda data: data.update(clock_period_ns=[5]),
                       lambda data: data["cases"][0].update(functional_equivalence=False)):
            after = fixture()
            change(after)
            with self.assertRaises(ValueError):
                compare(fixture(), after)

    def test_missing_and_invalid_metrics(self):
        for value in (None, True, -1, float("nan"), float("inf")):
            after = fixture()
            after["cases"][0]["light_hls"]["resources"]["ff"] = value
            with self.assertRaises(ValueError):
                compare(fixture(), after)

    def test_timing_failure(self):
        after = fixture()
        after["cases"][0]["light_hls"]["timing"]["worst_slack_ns"] = -0.1
        self.assertFalse(compare(fixture(), after)["all_timing_met"])
        after["cases"][0]["light_hls"]["timing"]["worst_slack_ns"] = None
        with self.assertRaises(ValueError):
            compare(fixture(), after)


if __name__ == "__main__":
    unittest.main()
