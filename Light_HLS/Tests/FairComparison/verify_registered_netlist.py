#!/usr/bin/env python3
"""Check routed functional netlists independently of static timing analysis."""
import argparse
import json
from pathlib import Path

from run_u250_fair_comparison import affinity_prefix, executable_path, run_command, sha256_file, tcl_brace


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--context", type=Path, required=True)
    parser.add_argument("--generator", choices=("light_hls", "vitis_hls"), required=True)
    args = parser.parse_args()
    root = args.context.resolve(strict=True)
    checkpoint = root / (args.generator + "_context") / "post_route.dcp"
    output = root / (args.generator + "_netlist")
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise ValueError("Refusing to overwrite netlist verification")
    vivado = executable_path("vivado", "Vivado")
    reference_path = root / "reference.log"
    bench = Path(__file__).with_name("registered_pressure_tb.v")
    result = {"valid": False, "generator": args.generator, "post_route_dcp_sha256": sha256_file(checkpoint),
              "testbench_sha256": sha256_file(bench), "reference_sha256": sha256_file(reference_path),
              "scope": "Post-route functional simulation with UNISIM; delays are checked separately by STA, not SDF"}
    try:
        script = output / "export.tcl"
        netlist = output / "netlist.v"
        script.write_text("\n".join(["set_param general.maxThreads 1",
            f"open_checkpoint {tcl_brace(checkpoint)}",
            f"write_verilog -force -mode funcsim {tcl_brace(netlist)}", "exit", ""]))
        run_command(affinity_prefix() + [str(vivado), "-mode", "batch", "-source", str(script)],
                    output, output / "export.log", 300)
        glbl = vivado.parent.parent / "data/verilog/src/glbl.v"
        if not glbl.is_file():
            raise ValueError("Vivado global simulation model is unavailable")
        run_command(affinity_prefix() + [str(executable_path("xvlog", "xvlog")), "--sv", "--relax",
                    str(netlist), str(bench), str(glbl)], output, output / "xvlog.log", 300)
        run_command(affinity_prefix() + [str(executable_path("xelab", "xelab")), "-mt", "off",
                    "-L", "unisims_ver", "-L", "unimacro_ver", "-s", "netlist_sim",
                    "registered_pressure_tb", "glbl"], output, output / "xelab.log", 600)
        run_command(affinity_prefix() + [str(executable_path("xsim", "xsim")), "netlist_sim", "-runall"],
                    output, output / "simulation.log", 600)
        lines = (output / "simulation.log").read_text().splitlines()
        reference = [line for line in reference_path.read_text().splitlines() if line.startswith("RESULT ")]
        trace = [line for line in lines if line.startswith("RESULT ")]
        if len(reference) != 8192 or trace != reference or not any("REGISTERED SHELL TESTS PASSED" in line for line in lines):
            raise ValueError("Routed functional netlist did not match all native C outputs")
        if sha256_file(checkpoint) != result["post_route_dcp_sha256"]:
            raise ValueError("Checkpoint changed during simulation")
        result.update(valid=True, reference_outputs=len(reference), netlist_sha256=sha256_file(netlist))
    except Exception as error:
        result["error"] = str(error)
        raise
    finally:
        (output / "netlist_verification.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result))


if __name__ == "__main__":
    main()
