#!/usr/bin/env python3
"""Archive routed timing coverage without rewriting measurements or eligibility."""
import argparse
import json
from pathlib import Path
from run_u250_fair_comparison import parse_route_metrics, sha256_file


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", action="append", required=True, help="label=comparison.json")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("Refusing to overwrite an implementation audit")
    cases = []
    for item in args.input:
        label, filename = item.split("=", 1)
        path = Path(filename).resolve(strict=True)
        report = json.loads(path.read_text())
        if not report["valid"] or not report["functional_equivalence"]["valid"]:
            raise ValueError("Only functionally checked, normalized comparisons can be audited")
        case = {"case": label, "report_sha256": sha256_file(path),
                "clock_period_ns": report["clock_period_ns"], "measurement_reward_eligible": report["reward_eligible"],
                "results": {}}
        for generator in ("light_hls", "vitis_hls"):
            directory = path.parent / "normalized_synthesis" / generator
            result = parse_route_metrics(directory, report["clock_period_ns"])
            result["artifact_sha256"] = {name: sha256_file(directory / name) for name in
                ("constraints.xdc", "post_route.dcp", "route_metrics.txt", "route_timing_summary.rpt", "route_status.rpt")}
            internal = directory / "internal_timing.txt"
            if internal.is_file():
                result["internal_register_timing_ns"] = dict((key, float(value)) for key, value in
                    (line.split("=", 1) for line in internal.read_text().splitlines()))
            case["results"][generator] = result
        cases.append(case)
    if len({item["case"] for item in cases}) != len(cases):
        raise ValueError("Duplicate case labels")
    result = {"valid": True, "measurement_reports_rewritten": False,
              "all_ooc_constraints_met": all(value["timing_met"] for case in cases for value in case["results"].values()),
              "note": "OOC virtual IO and clock boundaries are not a board implementation. Failed hold checks remain failures.",
              "cases": cases}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"cases": len(cases), "all_ooc_constraints_met": result["all_ooc_constraints_met"]}))


if __name__ == "__main__":
    main()
