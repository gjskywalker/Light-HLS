#!/usr/bin/env python3
"""Validate schedule-aware latency against retained and fresh RTL simulations."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[3]
PARSER = argparse.ArgumentParser(description=__doc__)
PARSER.add_argument("--binary", type=Path, required=True)
PARSER.add_argument("--output", type=Path, required=True)
OPTIONS, ARGS = PARSER.parse_known_args()
OPTIONS.binary = OPTIONS.binary.resolve()
OPTIONS.output = OPTIONS.output.resolve()
OPTIONS.output.mkdir(parents=True, exist_ok=False)
REFERENCE = ROOT / "runs/dapo_ii_pass_order_20261106_modern_final/evaluation"
OLD = ROOT / "runs/dapo_ii_pass_order_20261104_pinned/evaluation"


def read(path):
    return json.loads(path.read_text())


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, target, stem="driver"):
    target.mkdir(parents=True, exist_ok=True)
    command = [str(x) for x in command]
    result = subprocess.run(command, capture_output=True, text=True, timeout=240)
    (target / (stem + ".stdout.log")).write_text(result.stdout)
    (target / (stem + ".stderr.log")).write_text(result.stderr)
    (target / (stem + ".json")).write_text(json.dumps({
        "command": command, "exit_code": result.returncode}, indent=2) + "\n")
    return result


class ScheduledLatencyTests(unittest.TestCase):
    def invoke(self, ir, manifest, target, model="hybrid", estimate=False):
        command = [OPTIONS.binary, ir, "dut", "--manifest", manifest,
                   "--freq", "100MHz", "--board", "u250", "--output", target,
                   "--latency-model", model]
        if estimate:
            command.append("--estimate-only")
        return run(command, target)

    def test_frozen_ir_predictions_and_fresh_rtl(self):
        points = [(name, variant, REFERENCE) for name in ("substring", "getTanh", "bnn")
                  for variant in ("baseline", "rl_seed_0")]
        points.append(("substring", "rl_seed_0", OLD))
        summary = []
        for name, variant, root in points:
            with self.subTest(name=name, variant=variant, root=root):
                reference = root / name / variant
                candidate = read(reference / "candidate.json")
                target = OPTIONS.output / (root.parent.name + "_" + name + "_" + variant)
                result = self.invoke(candidate["ir"], candidate["backend_manifest"], target)
                self.assertEqual(result.returncode, 0, result.stderr[-3000:])
                report = read(target / "reports/estimate/dapo_estimate.json")
                prior = read(reference / "reports/estimate/pre_backend_estimate.json")
                self.assertEqual(report["latency_cycles"], prior["latency_cycles"])
                # Frozen IR remains a regression input, not a byte-exact RTL
                # snapshot across compiler revisions. Recheck actual behavior
                # against source and native IR whenever emission changes.
                validation = target / "fresh_validation"
                checked = run(["python3", ROOT / "Light_HLS/Tests/DAPOII/verify_rtl.py",
                    "--design", name, "--reports", target / "reports", "--ir", candidate["ir"],
                    "--output", validation, "--cases", "4"], target, "verify")
                self.assertEqual(checked.returncode, 0, checked.stdout[-3000:] + checked.stderr[-3000:])
                checked_report = read(validation / "verification.json")
                self.assertTrue(checked_report["rtl_correct"])
                self.assertEqual(checked_report["native_ir_equivalence"], "passed")
                cycles = [item["cycles"] for item in checked_report["cases"]]
                scheduled = report["scheduled_latency"]
                if scheduled["valid"]:
                    bounds = scheduled["cycles"]
                    self.assertTrue(all(bounds["minimum"] <= c <= bounds["maximum"] for c in cycles))
                    self.assertEqual(report["selected_latency_cycles"], bounds["maximum"])
                    if name == "bnn" or (name == "substring" and variant == "baseline"):
                        self.assertEqual(bounds["minimum"], cycles[0])
                        self.assertEqual(bounds["maximum"], cycles[0])
                    elif name == "substring":
                        self.assertLess(abs(bounds["maximum"] - cycles[0]),
                                        abs(prior["latency_cycles"] - cycles[0]))
                else:
                    self.assertEqual(report["selected_latency_model"], "heuristic_fallback")
                    self.assertEqual(report["selected_latency_cycles"], prior["latency_cycles"])
                summary.append({"name": name, "variant": variant, "reference": str(reference),
                    "heuristic": report["latency_cycles"], "scheduled": scheduled,
                    "rtl_cycles": cycles, "rtl_sha256": sha(target / "reports/rtl/top.v")})
                (OPTIONS.output / "comparison.json").write_text(json.dumps(summary, indent=2) + "\n")
        substring = [r for r in summary if r["name"] == "substring" and r["variant"] == "rl_seed_0"]
        self.assertEqual(len(substring), 2)
        newer, older = substring
        self.assertLess(older["scheduled"]["cycles"]["maximum"], newer["scheduled"]["cycles"]["maximum"])
        self.assertLess(older["rtl_cycles"][0], newer["rtl_cycles"][0])

    def test_estimate_only_binds_resources_without_emitting_rtl(self):
        for name in ("substring", "bnn", "getTanh"):
            with self.subTest(name=name):
                candidate = read(REFERENCE / name / "baseline/candidate.json")
                target = OPTIONS.output / ("estimate_only_" + name)
                result = self.invoke(candidate["ir"], candidate["backend_manifest"], target, estimate=True)
                self.assertEqual(result.returncode, 0, result.stderr[-3000:])
                audit = read(target / "reports/dapo_ir_audit.json")
                self.assertEqual(audit["pass_sequence"][-1], "HI_ResourceBinding")
                self.assertFalse((target / "reports/rtl/top.v").exists())
                report = read(target / "reports/estimate/dapo_estimate.json")
                self.assertIn("scheduled_latency", report)

    def fixture(self, name, body):
        target = OPTIONS.output / name
        target.mkdir()
        source = target / "fixture.cpp"
        source.write_text("unsigned dut(unsigned x) { " + body + " }\n")
        prepared = target / "prepared"
        result = run([OPTIONS.binary, "--prepare-source", source, "dut", "--freq", "100MHz",
                      "--board", "u250", "--output", prepared], prepared, "prepare")
        self.assertEqual(result.returncode, 0, result.stderr[-2000:])
        manifest = prepared / "reports/dapo_manifest.json"
        return target, manifest, read(manifest)["ir"]

    def test_bounded_dynamic_trip_count_is_an_interval(self):
        root, manifest, ir = self.fixture("unknown", "unsigned n=x&7; for(unsigned i=0;i<n;++i) x=x*3+1; return x;")
        strict = root / "strict"
        result = self.invoke(ir, manifest, strict, model="scheduled", estimate=True)
        self.assertEqual(result.returncode, 0, result.stderr[-2000:])
        report = read(strict / "reports/estimate/dapo_estimate.json")
        self.assertTrue(report["valid"])
        self.assertTrue(report["scheduled_latency"]["valid"])
        self.assertFalse(report["scheduled_latency"]["cycles"]["constant"])
        self.assertEqual(report["selected_latency_cycles"], report["scheduled_latency"]["cycles"]["maximum"])
        fallback = root / "hybrid"
        result = self.invoke(ir, manifest, fallback, estimate=True)
        self.assertEqual(result.returncode, 0, result.stderr[-2000:])
        report = read(fallback / "reports/estimate/dapo_estimate.json")
        self.assertEqual(report["selected_latency_model"], "scheduled_cfg_v1")
        self.assertTrue(report["scheduled_latency"]["valid"])

    def test_unproven_termination_is_still_rejected(self):
        root, manifest, ir = self.fixture("unbounded", "while(x!=1) x=x*3+1; return x;")
        strict = root / "strict"
        result = self.invoke(ir, manifest, strict, model="scheduled", estimate=True)
        self.assertNotEqual(result.returncode, 0)
        report = read(strict / "reports/estimate/dapo_estimate.json")
        self.assertFalse(report["valid"])
        self.assertIsNone(report["selected_latency_cycles"])
        self.assertIn("Nonconstant", report["scheduled_latency"]["reason"])

    def test_loop_shapes_against_fresh_iverilog(self):
        fixtures = {
            "pretest": ("for(unsigned i=0;i<5;++i) x=x*3+1; return x;", 5),
            "posttest": ("unsigned i=0; do {x=x*3+1; ++i;} while(i<5); return x;", 5),
            "nested": ("for(unsigned i=0;i<3;++i) for(unsigned j=0;j<4;++j) x=x*3+1; return x;", 12),
            "zero": ("for(unsigned i=0;i<0;++i) x=x*3+1; return x;", 0),
        }
        for name, (body, count) in fixtures.items():
            with self.subTest(name=name):
                root, manifest, ir = self.fixture(name, body)
                generated = root / "generated"
                result = self.invoke(ir, manifest, generated, model="scheduled")
                self.assertEqual(result.returncode, 0, result.stderr[-2000:])
                report = read(generated / "reports/estimate/dapo_estimate.json")
                self.assertTrue(report["scheduled_latency"]["valid"])
                bounds = report["scheduled_latency"]["cycles"]
                self.assertTrue(bounds["constant"])
                expected = 7
                for _ in range(count):
                    expected = (expected * 3 + 1) & 0xffffffff
                bench = root / "tb.v"
                bench.write_text('''module tb;
reg ap_clk=0; always #5 ap_clk=~ap_clk;
reg ap_rst=1, ap_start=0; reg [31:0] x=7;
wire ap_done, ap_idle, ap_ready; wire [31:0] ap_return;
dut u(.ap_clk(ap_clk),.ap_rst(ap_rst),.ap_start(ap_start),
      .ap_done(ap_done),.ap_idle(ap_idle),.ap_ready(ap_ready),.x(x),.ap_return(ap_return));
integer cycles=0;
initial begin
  repeat(3) @(negedge ap_clk); ap_rst=0;
  @(negedge ap_clk); ap_start=1;
  @(posedge ap_clk); #1;
  @(negedge ap_clk); ap_start=0;
  while(!ap_done && cycles<10000) begin @(posedge ap_clk); #1; cycles=cycles+1; end
  if(!ap_done || ap_return !== 32'dEXPECTED) $fatal(1,"Wrong result or timeout");
  $display("CYCLES=%0d",cycles); $finish;
end
endmodule
'''.replace("EXPECTED", str(expected)))
                result = run(["iverilog", "-g2012", "-s", "tb", "-o", root / "sim",
                              generated / "reports/rtl/top.v", bench], root, "iverilog")
                self.assertEqual(result.returncode, 0, result.stderr)
                result = run(["vvp", root / "sim"], root, "simulation")
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                measured = int(re.search(r"CYCLES=(\d+)", result.stdout).group(1))
                self.assertEqual(measured, bounds["maximum"])


if __name__ == "__main__":
    unittest.main(argv=[__file__] + ARGS)
