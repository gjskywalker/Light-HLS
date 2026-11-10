#!/usr/bin/env python3
"""Recheck unchanged synthesized RTL with a repaired simulation adapter.

The original comparison is never overwritten. Synthesis metrics retain their
original files and explicit provenance; this is not a new synthesis run.
"""

import argparse
import copy
import hashlib
import json
from pathlib import Path
import sys

from run_u250_fair_comparison import (
    FLOW_ID, executable_path, make_deltas, make_normalized_contract,
    parse_interface_signature, parse_light_schedule, parse_metrics,
    parse_vitis_estimate, run_simulation, scalar_partition_aliases, sha256_file,
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("--testbench", required=True, type=Path)
    parser.add_argument("--testbench-top", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--testbench-define", action="append")
    parser.add_argument("--vitis-testbench-define", action="append")
    parser.add_argument("--rtl-simulator", choices=("iverilog", "xsim"), default="iverilog")
    parser.add_argument("--reference-log", type=Path)
    args = parser.parse_args()
    original_path = args.report.resolve(strict=True)
    origin = original_path.parent
    original = json.loads(original_path.read_text())
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        parser.error("Output must be absent or empty")
    report = copy.deepcopy(original)
    report.update(valid=False, reward_eligible=False, errors=[])
    contract = original["input_fingerprints"]
    assert contract["flow_id"] == FLOW_ID
    assert sha256_file(Path(original["source"])) == original["source_sha256"]
    assert original.get("config_sha256") is None, "Explicit configuration is not supported"
    common = args.testbench_define if args.testbench_define is not None else contract["common_defines"]
    vitis = args.vitis_testbench_define if args.vitis_testbench_define is not None else contract["vitis_only_defines"]
    testbench = args.testbench.resolve(strict=True)
    report["testbench_sha256"] = sha256_file(testbench)
    identity = dict(contract, testbench_sha256=report["testbench_sha256"],
                    common_defines=common, vitis_only_defines=vitis, rtl_simulator=args.rtl_simulator)
    report["input_fingerprints"] = identity
    report["comparison_id"] = hashlib.sha256(json.dumps(identity, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    report["fairness_contract"]["simulation_adapter"] = {
        "common_defines": common, "vitis_only_defines": vitis, "rtl_simulator": args.rtl_simulator}
    light_reports = origin / "light_hls/reports"
    vitis_solution = origin / "vitis_hls/vitis_project/solution"
    rtls = {"light_hls": [light_reports / "rtl/top.v"],
            "vitis_hls": sorted((vitis_solution / "syn/verilog").glob("*.v"))}
    assert all(rtls.values())
    evidence = [original_path, *(path for files in rtls.values() for path in files)]
    metrics = {}
    for stage in ("common_synthesis", "normalized_synthesis"):
        metrics[stage] = {}
        for generator in rtls:
            directory = origin / stage / generator
            metric = directory / "metrics.txt"
            value = parse_metrics(metric, original["clock_period_ns"], 0)
            field = "post_rtl_synthesis" if stage == "normalized_synthesis" else "native_post_rtl_synthesis"
            value["runtime_seconds"] = original.get("results", {}).get(generator, {}).get(field, {}).get("runtime_seconds")
            value["reused_from"] = str(metric)
            metrics[stage][generator] = value
            for name in ("metrics.txt", "run_vivado.tcl", "vivado.log", "utilization.rpt", "timing_summary.rpt"):
                path = directory / name
                assert path.is_file(), path
                evidence.append(path)
            assert "Exiting Vivado" in (directory / "vivado.log").read_text()
    for generator in rtls:
        evidence.append(origin / "interface_normalization" / generator / "normalized_wrapper.v")
    fingerprints = {str(path): sha256_file(path) for path in evidence}
    report["synthesis_reuse"] = {
        "original_report": str(original_path), "original_comparison_id": original["comparison_id"],
        "original_executable_sha256": original["light_hls_executable_sha256"],
        "new_synthesis_run": False, "artifact_sha256": fingerprints,
        "note": "Unchanged existing RTL and synthesis artifacts; only simulation was rerun. Unknown original runtimes remain null."}
    native = metrics["common_synthesis"]
    normalized = metrics["normalized_synthesis"]
    aliases = scalar_partition_aliases(light_reports / "binding/binding.json",
        parse_interface_signature(native["light_hls"]["interface_signature"]),
        parse_interface_signature(native["vitis_hls"]["interface_signature"]))
    ports, _, _ = make_normalized_contract(native["light_hls"]["interface_signature"], native["vitis_hls"]["interface_signature"], aliases)
    for name, results in (("native_interface_equivalence", native), ("interface_equivalence", normalized)):
        signatures = {key: value["interface_signature"] for key, value in results.items()}
        report[name] = {"valid": signatures["light_hls"] == signatures["vitis_hls"], **signatures}
    report["interface_normalization"] = {"valid": report["interface_equivalence"]["valid"],
        "wrapper_top": original["top_function"] + "__normalized", "vitis_aliases": aliases,
        "contract": [dict(name=name, **ports[name]) for name in sorted(ports)]}
    simulations = {}
    for generator, files in rtls.items():
        directory = output / "simulation" / generator
        directory.mkdir(parents=True)
        simulations[generator] = run_simulation(executable_path("iverilog", "Icarus"), files,
            testbench, args.testbench_top, directory, 600, common + (vitis if generator == "vitis_hls" else []),
            args.rtl_simulator)
    matched = simulations["light_hls"]["trace"] == simulations["vitis_hls"]["trace"]
    if args.reference_log:
        reference = args.reference_log.read_text().splitlines()
        assert all(value["trace"] == reference for value in simulations.values()), "C-reference mismatch"
        report["reference_sha256"] = sha256_file(args.reference_log)
    report["functional_equivalence"] = {"requested": True, "valid": matched, "trace_match": matched, **simulations}
    report["results"] = {
        "light_hls": {"generator_runtime_seconds": None,
            "schedule": parse_light_schedule(light_reports / "scheduling/schedule.json",
                light_reports / "scheduling/pipeline_schedule.json", light_reports / "scheduling/dataflow_graph.json")},
        "vitis_hls": {"generator_runtime_seconds": None, "estimate": parse_vitis_estimate(vitis_solution / "syn/report/csynth.xml")}}
    for generator in rtls:
        report["results"][generator].update(native_post_rtl_synthesis=native[generator], post_rtl_synthesis=normalized[generator])
    report["deltas"] = make_deltas(normalized["light_hls"], normalized["vitis_hls"])
    assert fingerprints == {str(path): sha256_file(path) for path in evidence}, "Synthesis artifacts changed during validation"
    assert sha256_file(testbench) == report["testbench_sha256"], "Testbench changed during validation"
    report["valid"] = report["interface_equivalence"]["valid"] and matched
    report["reward_eligible"] = report["valid"]
    # Expose the immutable original artifacts to existing archive/identity tools.
    for name in ("light_hls", "vitis_hls", "common_synthesis", "normalized_synthesis", "interface_normalization"):
        (output / name).symlink_to(origin / name, target_is_directory=True)
    (output / "u250_fair_comparison.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Revalidated {original['top_function']}: {report['valid']}")
    return 0 if report["valid"] else 1


if __name__ == "__main__":
    sys.exit(main())
