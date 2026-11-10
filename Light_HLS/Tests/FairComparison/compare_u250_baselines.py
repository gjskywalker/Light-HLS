#!/usr/bin/env python3

"""Audit same-contract U250 checkpoints without hiding QoR tradeoffs."""

import argparse
from collections import Counter
import json
import math
from pathlib import Path


def finite_number(value, name):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"Missing numeric metric: {name}")
    if not math.isfinite(value) or value < 0:
        raise ValueError(f"Invalid metric: {name}={value}")
    return value


def index_cases(archive):
    if archive.get("valid") is not True or archive.get("reward_eligible") is not True:
        raise ValueError("Archive is not valid and reward-eligible")
    result = {}
    for case in archive.get("cases", []):
        key = (case.get("suite"), case.get("case"))
        if not all(key) or key in result:
            raise ValueError(f"Missing or duplicate case identity: {key}")
        for flag in ("valid", "reward_eligible", "functional_equivalence",
                     "normalized_interface_equivalence"):
            if case.get(flag) is not True:
                raise ValueError(f"{key}: {flag} is not true")
        result[key] = case
    if not result:
        raise ValueError("Empty archive")
    return result


def metrics(case):
    result = {"cycles": finite_number(case.get("simulation_cycles", {}).get("light_hls"),
                                      "simulation cycles")}
    resources = case.get("light_hls", {}).get("resources", {})
    for name in ("lut", "ff", "dsp", "bram_tiles"):
        result[name] = finite_number(resources.get(name), name)
    return result


def compare(baseline, candidate):
    for field in ("board", "part", "flow", "clock_period_ns"):
        if not baseline.get(field) or baseline[field] != candidate.get(field):
            raise ValueError(f"Comparison contract differs: {field}")
    old_cases, new_cases = index_cases(baseline), index_cases(candidate)
    if old_cases.keys() != new_cases.keys():
        raise ValueError("Case sets differ; missing cases must not be silently dropped")
    results = []
    for key, old in old_cases.items():
        new = new_cases[key]
        for field in ("source", "top", "source_sha256", "testbench_sha256", "config_sha256"):
            if old.get(field) != new.get(field):
                raise ValueError(f"{key}: input differs: {field}")
        for field in ("source_sha256", "testbench_sha256"):
            if not old.get(field):
                raise ValueError(f"{key}: missing fingerprint: {field}")
        for generator in ("light_hls", "vitis_hls"):
            for field in ("tool_version", "flow_id"):
                if (not old.get(generator, {}).get(field) or
                        old[generator][field] != new.get(generator, {}).get(field)):
                    raise ValueError(f"{key}: synthesis contract differs: {generator}/{field}")
        before, after = metrics(old), metrics(new)
        improved = [name for name in before if after[name] < before[name]]
        worsened = [name for name in before if after[name] > before[name]]
        timing = new.get("light_hls", {}).get("timing", {})
        slack = timing.get("worst_slack_ns")
        if slack is not None and (isinstance(slack, bool) or
                                  not isinstance(slack, (int, float)) or not math.isfinite(slack)):
            raise ValueError(f"{key}: invalid timing result")
        if timing.get("clock_constrained") is not True:
            raise ValueError(f"{key}: Light-HLS clock is not constrained")
        if slack is None and after["ff"] != 0:
            raise ValueError(f"{key}: registered Light-HLS design has no timing result")
        timing_met = slack is None or slack >= 0
        status = ("tradeoff" if improved and worsened else "improved" if improved else
                  "regressed" if worsened else "unchanged")
        results.append({"suite": key[0], "case": key[1], "status": status,
                        "before": before, "after": after,
                        "delta": {name: after[name] - before[name] for name in before},
                        "improved_metrics": improved, "worsened_metrics": worsened,
                        "timing_met": timing_met, "worst_slack_ns": slack})
    return {"schema_version": "1.0", "valid": True,
            "stage": "u250_checkpoint_qor_change_audit",
            "counts": dict(Counter(item["status"] for item in results)),
            "all_timing_met": all(item["timing_met"] for item in results),
            "no_metric_regressions": all(not item["worsened_metrics"] for item in results),
            "cases": results}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--candidate", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--require-no-regressions", action="store_true",
                        help="Fail on any cycle/resource increase, including a tradeoff")
    args = parser.parse_args()
    if args.output.exists():
        parser.error("Refusing to overwrite an existing audit")
    try:
        report = compare(json.loads(args.baseline.read_text()), json.loads(args.candidate.read_text()))
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f"QoR audit failed: {error}\n")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    for case in report["cases"]:
        changes = ", ".join(f"{name} {case['before'][name]}->{case['after'][name]}"
                            for name in ("cycles", "lut", "ff"))
        print(f"{case['suite']}/{case['case']}: {case['status']}: {changes}")
    print(json.dumps(report["counts"], sort_keys=True))
    return int(not report["all_timing_met"] or
               (args.require_no_regressions and not report["no_metric_regressions"]))


if __name__ == "__main__":
    raise SystemExit(main())
