#!/usr/bin/env python3
"""Validate the isolated DAPO IR driver without rebuilding production LightHLS."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest


PARSER = argparse.ArgumentParser(description=__doc__)
PARSER.add_argument("--binary", type=Path, required=True)
PARSER.add_argument("--prepared-root", type=Path, required=True)
PARSER.add_argument("--rtl-root", type=Path, required=True)
OPTIONS, UNITTEST_ARGS = PARSER.parse_known_args()
OPTIONS.binary = OPTIONS.binary.resolve()
OPTIONS.prepared_root = OPTIONS.prepared_root.resolve()
OPTIONS.rtl_root = OPTIONS.rtl_root.resolve()


def load_json(path):
    return json.loads(path.read_text())


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class DapoIRDriverTests(unittest.TestCase):
    """Check CLI failures, immutable preparation contracts, and backend replay."""

    @classmethod
    def setUpClass(cls):
        cls.manifest_path = OPTIONS.prepared_root / "getTanh/reports/dapo_manifest.json"
        cls.manifest = load_json(cls.manifest_path)
        cls.temporary = tempfile.TemporaryDirectory(prefix="lighthls-dapo-driver-tests-")
        cls.root = Path(cls.temporary.name)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def invoke(self, *, manifest=None, input_ir=None, extra=None, frequency=True):
        output = self.root / self.id().rsplit(".", 1)[-1]
        command = [str(OPTIONS.binary), str(input_ir or self.manifest["ir"]), "dut",
                   "--manifest", str(manifest or self.manifest_path),
                   "--output", str(output)]
        if frequency:
            command += ["--freq", "100MHz"]
        command += extra or []
        result = subprocess.run(command, capture_output=True, text=True, timeout=120)
        return result, output

    def mutated_manifest(self, key, value):
        manifest = dict(self.manifest)
        manifest[key] = value
        path = self.root / (self.id().rsplit(".", 1)[-1] + ".json")
        path.write_text(json.dumps(manifest))
        return path

    def test_frequency_is_required(self):
        result, _ = self.invoke(frequency=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Missing required option --freq", result.stderr)

    def test_unknown_flag_is_rejected(self):
        result, _ = self.invoke(extra=["--not-a-real-option", "1"])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Unknown option", result.stderr)

    def test_bad_ir_is_rejected(self):
        path = self.root / "invalid.ll"
        path.write_text("This is not LLVM IR.\n")
        result, output = self.invoke(input_ir=path)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Cannot parse LLVM IR", result.stderr)
        self.assertFalse((output / "reports/rtl/top.v").exists())

    def test_configuration_hash_mismatch_is_rejected(self):
        path = self.mutated_manifest("config_hash", "fnv1a64:0000000000000000")
        result, _ = self.invoke(manifest=path)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Effective configuration changed", result.stderr)

    def test_source_hash_mismatch_is_rejected(self):
        path = self.mutated_manifest("source_hash", "fnv1a64:0000000000000000")
        result, _ = self.invoke(manifest=path)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Source changed after preparation", result.stderr)

    def test_clock_mismatch_is_rejected(self):
        path = self.mutated_manifest("clock_period_ns", 4.0)
        result, _ = self.invoke(manifest=path)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Manifest clock mismatch", result.stderr)

    def test_unsupported_vector_has_early_audit(self):
        path = self.root / "vector.ll"
        path.write_text('target triple = "x86_64-pc-linux-gnu"\n'
                        'define i32 @dut(ptr %a) {\n'
                        '  %v = load <2 x i32>, ptr %a\n'
                        '  %x = extractelement <2 x i32> %v, i32 0\n'
                        '  ret i32 %x\n}\n')
        result, output = self.invoke(input_ir=path)
        self.assertNotEqual(result.returncode, 0)
        audit = load_json(output / "reports/dapo_ir_audit.json")
        self.assertEqual(audit["status"], "unsupported_ir")
        self.assertTrue((output / "reports/rtl/failure.json").is_file())
        self.assertFalse((output / "reports/rtl/top.v").exists())

    def test_all_source_depths_survive_ir_input(self):
        for name in ("substring", "getTanh", "bnn"):
            with self.subTest(design=name):
                manifest = load_json(OPTIONS.prepared_root / name / "reports/dapo_manifest.json")
                binding = load_json(OPTIONS.rtl_root / name / "reports/binding/binding.json")
                expected = {}
                for key, dimensions in manifest["dimensions"].items():
                    argument = key[len("dut-"):].rsplit("-", 1)[0]
                    depth = 1
                    for dimension in dimensions:
                        depth *= dimension
                    expected[argument] = depth
                for memory in binding["memory_objects"]:
                    self.assertEqual(memory["depth"], expected[memory["name"]])
                    self.assertEqual(memory["depth_source"], "frontend")
                self.assertEqual(len(binding["memory_objects"]), len(expected))

    def test_partial_unroll_is_materialized(self):
        for name, factor, count in (("substring", 4, 7), ("getTanh", 2, 1)):
            manifest = load_json(OPTIONS.prepared_root / name / "reports/dapo_manifest.json")
            self.assertEqual(len(manifest["applied_unroll"]), count)
            self.assertEqual(set(manifest["applied_unroll"]), set(manifest["fixed_unroll"]))
            for label in manifest["applied_unroll"]:
                self.assertEqual(manifest["fixed_unroll"][label], factor)
                remaining = [key for key, value in manifest["prepared_loop_labels"].items()
                             if value == label]
                self.assertEqual(len(remaining), 1)
                expected_trip = (4096 if name == "substring" else 1000) // factor
                self.assertEqual(manifest["prepared_loop_trip_counts"][remaining[0]], expected_trip)

    def test_outer_pipeline_descendant_is_fully_unrolled(self):
        manifest = load_json(OPTIONS.prepared_root / "bnn/reports/dapo_manifest.json")
        schedule = load_json(OPTIONS.rtl_root / "bnn/reports/scheduling/schedule.json")
        self.assertEqual(manifest["fixed_unroll"], {"Loop_dut_2": 100})
        self.assertEqual(manifest["applied_unroll"], ["Loop_dut_2"])
        self.assertNotIn("Loop_dut_2", manifest["prepared_loop_labels"].values())
        self.assertEqual(manifest["pipeline"], {"Loop_dut_1": 200})
        self.assertEqual(len(schedule["pipelines"]), 1)
        self.assertEqual(schedule["pipelines"][0]["requested_ii"], 200)
        self.assertEqual(schedule["pipelines"][0]["achieved_ii"], 200)

    def test_no_optional_post_search_optimizations(self):
        for name in ("substring", "getTanh", "bnn"):
            audit = load_json(OPTIONS.rtl_root / name / "reports/dapo_ir_audit.json")
            self.assertEqual(audit["optional_optimization_passes"], [])
            self.assertEqual(audit["post_search_legalization"],
                             ["mem2reg", "lower-switch", "loop-simplify", "lcssa"])
            self.assertEqual(audit["legalized_ir_hash"], audit["backend_ir_hash"])
            self.assertEqual(audit["status"], "rtl_generated")

    def test_replay_and_synthesis_default(self):
        result, output = self.invoke()
        self.assertEqual(result.returncode, 0, result.stderr[-2000:])
        reference = OPTIONS.rtl_root / "getTanh/reports/rtl/top.v"
        self.assertEqual(sha256(output / "reports/rtl/top.v"), sha256(reference))
        synthesis = load_json(output / "reports/vivado_report/vivado_report.json")
        self.assertFalse(synthesis["requested"])
        self.assertFalse(synthesis["attempted"])
        self.assertEqual(synthesis["status"], "not_requested")


if __name__ == "__main__":
    unittest.main(argv=[__file__] + UNITTEST_ARGS)
