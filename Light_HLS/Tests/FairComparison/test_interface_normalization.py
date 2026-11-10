#!/usr/bin/env python3

"""Unit tests for the fair-comparison interface normalizer."""

from __future__ import annotations

import importlib.util
import json
import subprocess
import tempfile
from pathlib import Path


SCRIPT_DIRECTORY = Path(__file__).resolve().parent
MODULE_PATH = SCRIPT_DIRECTORY / "run_u250_fair_comparison.py"
SPEC = importlib.util.spec_from_file_location("fair_comparison", MODULE_PATH)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def main() -> int:
    light_signature = [
        "ap_clk|IN|1",
        "ap_done|OUT|1",
        "input_0|IN|32",
        "input_1|IN|32",
        "output_r_address0|OUT|6",
        "output_r_d0|OUT|32",
    ]
    vitis_signature = [
        "ap_done|OUT|1",
        "input_0_0|IN|32",
        "input_0_1|IN|32",
        "output_r_address0|OUT|6",
        "output_r_address1|OUT|6",
        "output_r_d0|OUT|32",
        "output_r_d1|OUT|32",
        "output_r_q0|IN|32",
        "output_r_q1|IN|32",
    ]
    with tempfile.TemporaryDirectory(prefix="light-hls-interface-test.") as temporary:
        root = Path(temporary)
        binding = root / "binding.json"
        binding.write_text(
            json.dumps(
                {
                    "memory_objects": [
                        {
                            "name": "input",
                            "port_name": "input",
                            "storage": "external",
                            "bank_interface": "scalar",
                            "dimensions": [1, 2],
                            "banks": 2,
                            "access_mode": "read_only",
                        }
                    ]
                }
            ),
            encoding="utf-8",
        )
        light_ports = MODULE.parse_interface_signature(light_signature)
        vitis_ports = MODULE.parse_interface_signature(vitis_signature)
        aliases = MODULE.scalar_partition_aliases(
            binding, light_ports, vitis_ports
        )
        assert aliases == {"input_0_0": "input_0", "input_0_1": "input_1"}
        contract, light_mapping, vitis_mapping = MODULE.make_normalized_contract(
            light_signature, vitis_signature, aliases
        )
        assert "ap_clk" in contract
        assert "output_r_address1" in contract
        assert vitis_mapping["input_0_1"] == "input_1"

        light_native = root / "light_native.v"
        light_native.write_text(
            "module kernel(input ap_clk, output ap_done, input [31:0] input_0, "
            "input [31:0] input_1, output [5:0] output_r_address0, "
            "output [31:0] output_r_d0); assign ap_done=1'b1; "
            "assign output_r_address0=0; assign output_r_d0=input_0+input_1; endmodule\n",
            encoding="utf-8",
        )
        light_wrapper = root / "light_wrapper.v"
        MODULE.write_interface_wrapper(
            light_wrapper,
            "kernel__normalized",
            "kernel",
            contract,
            light_signature,
            light_mapping,
        )
        subprocess.run(
            [
                "iverilog",
                "-g2012",
                "-s",
                "kernel__normalized",
                "-o",
                str(root / "normalized.out"),
                str(light_native),
                str(light_wrapper),
            ],
            check=True,
        )
        wrapper_text = light_wrapper.read_text(encoding="utf-8")
        assert "assign output_r_address1 = 6'd0;" in wrapper_text
        assert "assign output_r_d1 = 32'd0;" in wrapper_text
    print("Fair-comparison interface-normalization tests passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
