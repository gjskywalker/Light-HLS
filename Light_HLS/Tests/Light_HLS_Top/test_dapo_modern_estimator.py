#!/usr/bin/env python3
"""Check the modern estimate-only contract against retained full-RTL estimates."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import unittest


PARSER = argparse.ArgumentParser(description=__doc__)
PARSER.add_argument("--binary", type=Path, required=True)
PARSER.add_argument("--prepared-root", type=Path, required=True)
PARSER.add_argument("--reference-root", type=Path, required=True)
PARSER.add_argument("--output", type=Path, required=True)
OPTIONS, UNITTEST_ARGS = PARSER.parse_known_args()
for name in ("binary", "prepared_root", "reference_root", "output"):
    setattr(OPTIONS, name, getattr(OPTIONS, name).resolve())
OPTIONS.output.mkdir(parents=True, exist_ok=False)


def read_json(path):
    return json.loads(path.read_text())


def stable_hash(path):
    value = 14695981039346656037
    for byte in path.read_bytes():
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return "fnv1a64:%016x" % value


class ModernEstimatorTests(unittest.TestCase):
    def invoke(self, design="getTanh", *, extra=None, ir=None, manifest=None,
               estimate=True, prepare=False):
        manifest_path = manifest or OPTIONS.prepared_root / design / "reports/dapo_manifest.json"
        metadata = read_json(manifest_path)
        output = OPTIONS.output / (self.id().rsplit(".", 1)[-1] + "_" + design)
        command = [str(OPTIONS.binary)]
        if prepare:
            command.append("--prepare-source")
        command += [str(ir or metadata["ir"]), "dut", "--manifest", str(manifest_path),
                    "--board", "u250", "--freq", "100MHz", "--output", str(output)]
        if estimate:
            command.append("--estimate-only")
        command += extra or []
        result = subprocess.run(command, capture_output=True, text=True, timeout=180)
        output.mkdir(parents=True, exist_ok=True)
        (output / "stdout.log").write_text(result.stdout)
        (output / "stderr.log").write_text(result.stderr)
        (output / "command.json").write_text(json.dumps({"command": command,
            "exit_code": result.returncode}, indent=2) + "\n")
        return result, output

    def test_all_three_match_retained_full_backend_estimates(self):
        for design in ("substring", "getTanh", "bnn"):
            with self.subTest(design=design):
                result, output = self.invoke(design)
                self.assertEqual(result.returncode, 0, result.stderr[-3000:])
                actual = read_json(output / "reports/estimate/dapo_estimate.json")
                expected = read_json(OPTIONS.reference_root / design /
                                     "baseline/reports/estimate/pre_backend_estimate.json")
                self.assertTrue(actual["valid"])
                self.assertEqual(actual["status"], "estimate_completed")
                self.assertEqual(actual["schema_version"], "1.0")
                self.assertEqual(actual["evaluator"], "HI_WithDirectiveTimingResourceEvaluation")
                self.assertEqual(actual["latency_cycles"], expected["latency_cycles"])
                self.assertEqual(actual["estimated_resources"], expected["resources"])
                self.assertEqual(actual["board"], "u250")
                self.assertEqual(actual["clock_period_ns"], 10)
                self.assertEqual(actual["target_frequency_mhz"], 100)
                self.assertEqual(actual["input_hash"], stable_hash(Path(actual["input"])))
                self.assertEqual(actual["manifest_hash"], stable_hash(Path(actual["manifest"])))
                self.assertEqual(actual["config_hash"], stable_hash(Path(actual["config"])))
                self.assertTrue(actual["validation"]["library_matches_manifest"])
                self.assertTrue(actual["validation"]["clock_matches_manifest"])
                self.assertEqual(actual["legalized_ir_hash"], actual["estimated_ir_hash"])
                audit = read_json(output / "reports/dapo_ir_audit.json")
                self.assertEqual(audit["status"], "estimate_completed")
                self.assertNotIn("HI_OperationScheduling", audit["pass_sequence"])
                self.assertFalse((output / "reports/rtl/top.v").exists())
                self.assertFalse((output / "reports/scheduling/schedule.json").exists())

    def test_estimate_only_rejects_synthesis(self):
        result, _ = self.invoke(extra=["--synthesis", "enable"])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cannot be combined", result.stderr)

    def test_previous_winners_match_retained_full_backend_estimates(self):
        for design in ("substring", "bnn"):
            with self.subTest(design=design):
                reference = OPTIONS.reference_root / design / "rl_seed_0"
                candidate = read_json(reference / "candidate.json")
                result, output = self.invoke(design, ir=Path(candidate["ir"]))
                self.assertEqual(result.returncode, 0, result.stderr[-3000:])
                actual = read_json(output / "reports/estimate/dapo_estimate.json")
                expected = read_json(reference / "reports/estimate/pre_backend_estimate.json")
                self.assertTrue(actual["valid"])
                self.assertEqual(actual["latency_cycles"], expected["latency_cycles"])
                self.assertEqual(actual["estimated_resources"], expected["resources"])

    def test_estimate_only_rejects_source_preparation(self):
        result, _ = self.invoke(prepare=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not --prepare-source", result.stderr)

    def test_repeated_flag_is_rejected(self):
        result, _ = self.invoke(extra=["--estimate-only"])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Repeated option", result.stderr)

    def test_unsupported_ir_has_no_reward(self):
        source = OPTIONS.output / "unsupported_vector.ll"
        source.write_text('target triple = "x86_64-pc-linux-gnu"\n'
                          'define i32 @dut(ptr %a) {\n'
                          '  %v = load <2 x i32>, ptr %a\n'
                          '  %x = extractelement <2 x i32> %v, i32 0\n'
                          '  ret i32 %x\n}\n')
        result, output = self.invoke(ir=source)
        self.assertNotEqual(result.returncode, 0)
        estimate = read_json(output / "reports/estimate/dapo_estimate.json")
        self.assertFalse(estimate["valid"])
        self.assertIsNone(estimate["latency_cycles"])
        self.assertEqual(estimate["status"], "unsupported_ir")
        self.assertFalse((output / "reports/rtl/top.v").exists())

    def test_pipeline_ii_changes_estimate(self):
        original = read_json(OPTIONS.prepared_root / "bnn/reports/dapo_manifest.json")
        config = OPTIONS.output / "pipeline_ii_201.cfg"
        config.write_text(Path(original["config"]).read_text().replace("II=200", "II=201"))
        original["config"] = str(config)
        original["config_hash"] = stable_hash(config)
        original["pipeline"]["Loop_dut_1"] = 201
        manifest = OPTIONS.output / "pipeline_ii_201.json"
        manifest.write_text(json.dumps(original))
        result, output = self.invoke("bnn", manifest=manifest)
        self.assertEqual(result.returncode, 0, result.stderr[-3000:])
        estimate = read_json(output / "reports/estimate/dapo_estimate.json")
        expected = read_json(OPTIONS.reference_root / "bnn/baseline/reports/estimate/pre_backend_estimate.json")
        self.assertEqual(estimate["pragma_configuration"]["pipeline"]["Loop_dut_1"], 201)
        self.assertGreater(estimate["latency_cycles"], expected["latency_cycles"])

    def test_library_metadata_mismatch_has_no_reward(self):
        original = read_json(OPTIONS.prepared_root / "getTanh/reports/dapo_manifest.json")
        original["library"] = str(Path(original["library"]).parents[1])
        manifest = OPTIONS.output / "mismatched_library.json"
        manifest.write_text(json.dumps(original))
        result, output = self.invoke(manifest=manifest)
        self.assertNotEqual(result.returncode, 0)
        estimate = read_json(output / "reports/estimate/dapo_estimate.json")
        self.assertFalse(estimate["valid"])
        self.assertFalse(estimate["validation"]["library_matches_manifest"])
        self.assertIsNone(estimate["latency_cycles"])

    def test_normal_rtl_generation_is_unchanged(self):
        result, output = self.invoke(estimate=False)
        self.assertEqual(result.returncode, 0, result.stderr[-3000:])
        reference = OPTIONS.reference_root / "getTanh/baseline/reports/rtl/top.v"
        generated = output / "reports/rtl/top.v"
        self.assertEqual(hashlib.sha256(generated.read_bytes()).hexdigest(),
                         hashlib.sha256(reference.read_bytes()).hexdigest())
        estimate = read_json(output / "reports/estimate/dapo_estimate.json")
        backend = read_json(output / "reports/estimate/pre_backend_estimate.json")
        self.assertEqual(estimate["latency_cycles"], backend["latency_cycles"])
        self.assertEqual(estimate["estimated_resources"], backend["resources"])


if __name__ == "__main__":
    unittest.main(argv=[__file__] + UNITTEST_ARGS)
