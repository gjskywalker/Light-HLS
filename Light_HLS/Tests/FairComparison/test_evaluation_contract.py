#!/usr/bin/env python3
"""Reject ambiguous throughput traces and check the constrained flow recipe."""
from pathlib import Path
import tempfile
import unittest
from run_u250_fair_comparison import FLOW_ID, write_common_vivado_tcl
from run_sustained_throughput import summarize
from run_registered_context import FLOW_ID as REGISTERED_FLOW_ID, resource_counts, write_flow, write_shell


class EvaluationContract(unittest.TestCase):
    def test_registered_context_is_a_distinct_physical_contract(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_shell(root / "shell.v", False)
            shell = (root / "shell.v").read_text()
            self.assertIn("BUFG clock_buffer", shell)
            self.assertIn("input_r_q0 <= (input_r_address0 * 131)", shell)
            self.assertNotIn(".input_r_we0", shell)
            write_shell(root / "shell.v", True)
            self.assertIn(".input_r_we0", (root / "shell.v").read_text())
            write_flow(root, [root / "shell.v"], 4)
            script = (root / "run.tcl").read_text()
            xdc = (root / "constraints.xdc").read_text()
            self.assertIn("Unexpected unconstrained external interface", script)
            self.assertIn("physical global clock buffer is required", script)
            self.assertIn("kernel_utilization.rpt", script)
            self.assertIn("\nphys_opt_design\n", script)
            self.assertNotIn("phys_if", script)
            self.assertEqual(script.count("Unexpected unconstrained external interface"), 1)
            self.assertIn("io_budget_fraction=0\"", script)
            self.assertEqual(xdc, "create_clock -name ap_clk -period 4 [get_ports ap_clk]\n")
            self.assertNotIn("set_false_path", script + xdc)
            self.assertNotIn("set_multicycle_path", script + xdc)
            self.assertNotEqual(FLOW_ID, REGISTERED_FLOW_ID)

    def test_hierarchical_resources_fail_closed(self):
        report = "| CLB LUTs | 10 |\n| CLB Registers | 20 |\n| DSPs | 1 |\n| Block RAM Tile | 0.5 |"
        self.assertEqual(resource_counts(report), {"lut": 10, "ff": 20, "dsp": 1, "bram_tiles": 0.5})
        with self.assertRaises(ValueError):
            resource_counts("| CLB LUTs | 0 |")

    def test_constraints_before_synthesis_and_separate_route_artifacts(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_common_vivado_tcl(root / "run.tcl", [root / "top.v"], "top", 4, root, True)
            script = (root / "run.tcl").read_text()
            self.assertLess(script.index("read_xdc -unmanaged"), script.index("synth_design"))
            self.assertLess(script.index("post_synth.dcp"), script.index("place_design"))
            self.assertLess(script.index("route_design"), script.index("post_route.dcp"))
            self.assertIn("route_metrics.txt", script)
            xdc = (root / "constraints.xdc").read_text()
            self.assertIn("-period 4", xdc)
            self.assertIn("set_input_delay -clock ap_clk -max 0.8", xdc)
            self.assertIn("set_output_delay -clock ap_clk -min 0", xdc)
            self.assertNotEqual(FLOW_ID, "vivado_ooc_normalized_v2")

    def test_commit_intervals_not_acknowledgement_latency(self):
        lines = [f"SUSTAINED trial={i} acknowledged={10+i*260} last_commit={270+i*260} done={269+i*260}"
                 for i in range(32)] + ["SUSTAINED TESTS PASSED"]
        metrics = summarize(lines)
        self.assertEqual(metrics["steady_commit_interval_cycles"], 260)
        self.assertAlmostEqual(metrics["steady_outputs_per_cycle"], 256 / 260)
        for bad in (lines[:30], lines + [lines[0]], list(reversed(lines))):
            with self.assertRaises(ValueError):
                summarize(bad)


if __name__ == "__main__":
    unittest.main()
