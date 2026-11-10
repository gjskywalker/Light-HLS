#!/usr/bin/env python3
"""Unit tests for benchmark domains and the generated memory checker."""

import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


MODULE_PATH = Path(__file__).with_name("verify_rtl.py")
SPEC = importlib.util.spec_from_file_location("verify_rtl", MODULE_PATH)
VERIFY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFY)


class VerificationTests(unittest.TestCase):
    def test_vectors_are_deterministic_and_bounded(self):
        for design, spec in VERIFY.SPECS.items():
            for trial in range(4):
                case = VERIFY.make_case(design, trial, 17)
                self.assertEqual(case, VERIFY.make_case(design, trial, 17))
                for name, depth in spec["arguments"]:
                    if depth:
                        self.assertEqual(len(case[name]), depth)
                        self.assertTrue(all(-(2**31) <= value < 2**31 for value in case[name]))
                if design == "bnn":
                    self.assertTrue(all(0 <= address < 10000 for address in case["addr_in"]))
                    if trial == 2:
                        self.assertLessEqual(len(set(case["addr_in"])), 32)
                if design == "getTanh":
                    self.assertTrue(all(angle >= 0 for angle in case["A"]))
                    self.assertEqual(case["cosh"], [4096] * 5)
                    if trial < 3:
                        self.assertTrue(all(address < 500 for address in case["addr_in"]))
                        self.assertTrue(all(address >= 500 for address in case["addr_out"]))

    def test_port_parser(self):
        name, ports = VERIFY.parse_ports("module kernel (input wire ap_clk, output reg [31:0] ap_return); endmodule")
        self.assertEqual(name, "kernel")
        self.assertEqual(ports["ap_return"], ("output", 32))

    def test_wrapper_preserves_argument_order(self):
        text = VERIFY.wrapper_source(VERIFY.SPECS["bnn"], "_Z3dutPiiS_S_S_S_S_")
        self.assertIn(' __asm__("_Z3dutPiiS_S_S_S_S_")', text)
        self.assertIn("dut(addr_in.data(), alpha, w.data(), addr_out.data(), data.data(), in.data(), mean.data())", text)

    def test_bench_uses_synchronous_read_first_memory(self):
        spec = {"arguments": [("A", 4)], "writable": ["A"]}
        ports = """module dut(input wire ap_clk, input wire ap_rst, input wire ap_start,
            output reg ap_done, output wire ap_ready, output reg ap_idle,
            output reg [31:0] ap_return, output reg [1:0] A_address0,
            output reg A_ce0, output reg A_we0, output reg [31:0] A_d0,
            input wire [31:0] A_q0); endmodule"""
        memories = [{"name": "A", "storage": "external", "protocol": "ap_memory",
                     "banks": 1, "ports": 1, "element_width": 32, "read_latency": 1}]
        with tempfile.TemporaryDirectory() as temporary:
            text = VERIFY.testbench_source(spec, memories, ports,
                                           [{"inputs": {"A": [1, 2, 3, 4]}, "return": 0}],
                                           Path(temporary), 1000)
        self.assertIn("A_q0 <= mem_A[A_address0];", text)
        self.assertIn("mem_A[A_address0] <= A_d0;", text)
        self.assertIn("$unsigned(A_address0) >= 4", text)
        self.assertIn("if (mem_A[index] !== expected_A[index])", text)
        self.assertIn("observed_done - accepted", text)

    @unittest.skipUnless(shutil.which("iverilog") and shutil.which("vvp"), "Icarus Verilog is unavailable")
    def test_icarus_memory_checker_end_to_end(self):
        spec = {"arguments": [("A", 4), ("alpha", 0)], "writable": ["A"]}
        rtl = """module dut(input wire ap_clk, input wire ap_rst, input wire ap_start,
            output reg ap_done, output wire ap_ready, output reg ap_idle,
            output reg [31:0] ap_return, output reg [1:0] A_address0,
            output reg A_ce0, output reg A_we0, output reg [31:0] A_d0,
            input wire [31:0] A_q0, input wire [31:0] alpha);
            reg [1:0] state, index;
            reg [31:0] captured_alpha;
            assign ap_ready = !ap_rst && state == 0;
            always @* begin
              A_address0 = index; A_ce0 = !ap_rst && (state == 1 || state == 2);
              A_we0 = state == 2; A_d0 = A_q0 + captured_alpha;
            end
            always @(posedge ap_clk) begin
              if (ap_rst) begin
                state <= 0; index <= 0; ap_done <= 0; ap_idle <= 1; ap_return <= 0;
              end else begin
                ap_done <= 0;
                case (state)
                  0: if (ap_start) begin
                    state <= 1; index <= 0; captured_alpha <= alpha; ap_idle <= 0;
                  end
                  1: state <= 2;
                  2: if (index == 3) state <= 3;
                     else begin index <= index + 1; state <= 1; end
                  3: begin state <= 0; ap_done <= 1; ap_idle <= 1; ap_return <= 7; end
                endcase
              end
            end
            endmodule"""
        memories = [{"name": "A", "storage": "external", "protocol": "ap_memory",
                     "banks": 1, "ports": 1, "element_width": 32, "read_latency": 1}]
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            cases = []
            for trial in range(3):
                case_root = output / f"case_{trial:03d}"
                case_root.mkdir()
                values = [trial + i for i in range(4)]
                alpha = trial + 2
                VERIFY.write_hex(case_root / "input_A.hex", values)
                VERIFY.write_hex(case_root / "expected_A.hex", [value + alpha for value in values])
                cases.append({"inputs": {"A": values, "alpha": alpha}, "return": 7})
            rtl_path, bench_path, executable = output / "top.v", output / "tb.v", output / "simulation"
            rtl_path.write_text(rtl)
            bench_path.write_text(VERIFY.testbench_source(spec, memories, rtl, cases, output, 100))
            subprocess.run(["iverilog", "-g2012", "-s", "dapoii_tb", "-o", str(executable),
                            str(rtl_path), str(bench_path)], check=True, capture_output=True, timeout=10)
            result = subprocess.run(["vvp", str(executable)], check=True, capture_output=True, text=True, timeout=10)
            VERIFY.write_hex(output / "case_000/expected_A.hex", [0, 0, 0, 0])
            mismatch = subprocess.run(["vvp", str(executable)], check=False, capture_output=True, text=True, timeout=10)
        self.assertIn("DAPOII RTL VERIFICATION PASSED: 3 cases", result.stdout)
        self.assertEqual(result.stdout.count("reads=4 writes=4"), 3)
        self.assertNotEqual(mismatch.returncode, 0)
        self.assertIn("Memory mismatch trial=0", mismatch.stdout)


if __name__ == "__main__":
    unittest.main()
