#!/usr/bin/env python3
"""Fail closed before archiving the September 29 correctness checkpoint."""
import argparse
import json
from pathlib import Path
import re
import shutil

from run_registered_context import FLOW_ID, resource_counts
from run_u250_fair_comparison import parse_route_metrics, sha256_file


def checked_json(path):
    data = json.loads(path.read_text())
    if data.get("valid") is not True:
        raise ValueError(f"Incomplete or failed result: {path}")
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--regression-dir", type=Path, help="Complete verified suite run; defaults to RUN_ROOT/release-complete")
    args = parser.parse_args()
    root = args.run_root.resolve(strict=True)
    output = args.output.resolve()
    if output.exists() and any(output.iterdir()):
        raise ValueError("Refusing to overwrite an archive")
    compiler_hash = sha256_file(root / "Light_HLS_Top-release")
    regression_dir = args.regression_dir.resolve(strict=True) if args.regression_dir else root / "release-complete"
    regressions = checked_json(regression_dir / "local_regressions.json")
    if not (regressions["expected_suites"] == regressions["completed_suites"] == len(regressions["results"]) == 27):
        raise ValueError("Incomplete regression matrix")
    if regressions["light_hls_executable_sha256"] != compiler_hash:
        raise ValueError("Regression compiler mismatch")
    for result in regressions["results"]:
        if result["status"] != "PASS" or sha256_file(Path(result["script"])) != result["script_sha256"]:
            raise ValueError("Regression script changed after verification")
    pressure = checked_json(root / "release-pressure/summary.json")
    apps = checked_json(root / "release-app-smoke/app_smoke.json")
    if (len(pressure["results"]) != 8 or pressure["expected_results"] != 8 or len(apps["results"]) != 18 or
            pressure["executable_sha256"] != compiler_hash or apps["executable_sha256"] != compiler_hash):
        raise ValueError("Incomplete or mismatched pressure/App matrix")
    for result in pressure["results"]:
        if result["case"] in ("dsp_map", "dsp_dataflow"):
            if result.get("sustained_calls") != 32 or result.get("reset_recovery_calls") != 32:
                raise ValueError("Missing sustained or reset recovery coverage")
    negative = root / "before-held-sim.log"
    if "Sustained output mismatch: 256" not in negative.read_text():
        raise ValueError("Missing pre-fix negative control")
    copies = {
        "local_regressions.json": regression_dir / "local_regressions.json",
        "local_regressions.rpt": regression_dir / "local_regressions.rpt",
        "pressure.json": root / "release-pressure/summary.json",
        "app_smoke.json": root / "release-app-smoke/app_smoke.json",
        "transaction_edges.json": regression_dir / "TransactionEdges/summary.json",
        "negative_control.log": negative,
        "negative_control_rtl.v": root / "before-reproduction/reports/rtl/top.v",
    }
    physical = {}
    for label in ("map-100", "bank-250", "dataflow-250"):
        directory = root / ("final-" + label)
        report_path = directory / "registered_context.json"
        report = checked_json(report_path)
        if (report.get("physical_valid") is not True or report["flow_id"] != FLOW_ID or
                report["compiler_sha256"] != compiler_hash or
                set(report["results"]) != {"light_hls", "vitis_hls"}):
            raise ValueError("Invalid physical comparison")
        if sha256_file(Path(report["source"])) != report["source_sha256"]:
            raise ValueError("Kernel source changed after verification")
        copies[f"{label}/registered_context.json"] = report_path
        copies[f"{label}/source.cpp"] = Path(report["source"])
        copies[f"{label}/registered_shell.v"] = directory / "registered_shell.v"
        physical[label] = {}
        for generator, entry in report["results"].items():
            if entry["functional_valid"] is not True or entry["reference_outputs"] != 8192:
                raise ValueError("Missing functional reference validation")
            for name, expected_hash in entry["rtl_sha256"].items():
                if sha256_file(Path(name)) != expected_hash:
                    raise ValueError(f"RTL changed: {name}")
            context = directory / (generator + "_context")
            measured = parse_route_metrics(context, report["clock_period_ns"])
            archived = entry["post_route"]
            kernel = resource_counts((context / "kernel_utilization.rpt").read_text())
            required = "bram_tiles" if label == "bank-250" else "dsp"
            if (not measured["timing_met"] or kernel[required] <= 0 or kernel != archived["kernel_resources"] or
                    measured["timing"]["worst_slack_ns"] != archived["timing"]["worst_slack_ns"] or
                    measured["timing"]["worst_hold_slack_ns"] != archived["timing"]["worst_hold_slack_ns"]):
                raise ValueError("Physical report mismatch or failed timing")
            constraints = (context / "constraints.xdc").read_text()
            if constraints != f"create_clock -name ap_clk -period {report['clock_period_ns']:.12g} [get_ports ap_clk]\n":
                raise ValueError("Unexpected timing exceptions or physical contract")
            if measured["interface_signature"] != ["ap_clk|IN|1"]:
                raise ValueError("Unexpected external IO requires new constraints")
            drc = (context / "route_drc.rpt").read_text()
            if re.search(r"\|\s*(?:Error|Critical Warning)\s*\|", drc, re.IGNORECASE):
                raise ValueError("Unresolved severe implementation DRC")
            physical[label][generator] = {"setup_slack_ns": measured["timing"]["worst_slack_ns"],
                "hold_slack_ns": measured["timing"]["worst_hold_slack_ns"],
                "kernel_resources": kernel, "shell_resources": archived["shell_resources"],
                "post_route_dcp_sha256": sha256_file(context / "post_route.dcp")}
            netlist = directory / (generator + "_netlist") / "netlist_verification.json"
            verification = checked_json(netlist)
            if (verification["post_route_dcp_sha256"] != physical[label][generator]["post_route_dcp_sha256"] or
                    verification["reference_outputs"] != 8192 or
                    verification["reference_sha256"] != sha256_file(directory / "reference.log") or
                    verification["netlist_sha256"] != sha256_file(netlist.parent / "netlist.v")):
                raise ValueError("Netlist verification does not match the routed implementation")
            copies[f"{label}/{generator}/netlist_verification.json"] = netlist
            physical[label][generator]["routed_functional_outputs"] = verification["reference_outputs"]
            for name in ("run.tcl", "constraints.xdc", "route_metrics.txt", "route_status.rpt",
                         "route_timing_summary.rpt", "route_drc.rpt", "kernel_utilization.rpt",
                         "hierarchical_utilization.rpt", "clock_utilization.rpt", "hold_paths.rpt", "setup_paths.rpt"):
                copies[f"{label}/{generator}/{name}"] = context / name
    for label in ("map-100", "dataflow-250"):
        run_name = "sustained-map-current" if label == "map-100" else "sustained-" + label
        path = root / run_name / "sustained_throughput.json"
        throughput = checked_json(path)
        if set(throughput["results"]) != {"light_hls", "vitis_hls"}:
            raise ValueError("Incomplete cross-generator stream comparison")
        copies[f"sustained-{label}.json"] = path
    # Nothing is copied until all validation gates have passed.
    output.mkdir(parents=True, exist_ok=True)
    for name, source in copies.items():
        target = output / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
    manifest = {"valid": True, "scope": "Bounded RTL correctness and registered on-chip physical context; no RL",
                "compiler_sha256": compiler_hash, "raw_run_root": str(root), "physical": physical,
                "pre_fix_compiler_sha256": sha256_file(root / "Light_HLS_Top-before"),
                "artifact_sha256": {name: sha256_file(output / name) for name in copies},
                "virtual_io_failure_waived": False, "board_timing_signoff": False}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps({"valid": True, "artifacts": len(copies)}))


if __name__ == "__main__":
    main()
