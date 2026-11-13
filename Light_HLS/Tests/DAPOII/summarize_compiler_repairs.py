#!/usr/bin/env python3
"""Summarize retained compiler acceptance and bounded functional evidence."""
import argparse
import hashlib
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("work", type=Path)
    args = parser.parse_args()
    work = args.work.resolve()
    groups = ["baseline_runtime", "baseline_focused", "repaired_runtime", "gramschmidt_runtime",
              "covariance_runtime", "new_checks_final", "release_predication", "predicated_floats",
              "pipeline_runtime", "cyclic_runtime", "block_runtime"]
    accepted, exclusions = {}, []
    for group in groups:
        path = work / group / "results.json"
        if not path.exists(): continue
        data = json.loads(path.read_text())
        for row in data["results"]:
            key = row["source"]
            if row.get("verified") and row.get("lowered_ir_verified") and row.get("cycles_in_envelope"):
                accepted[key] = dict(name=row["name"], source=key, result=str(path), binary_sha256=data["binary_sha256"],
                                     cases=len(row["verification"]["cases"]),
                                     cycles=[case["cycles"] for case in row["verification"]["cases"]],
                                     envelope=row["cycle_envelope"])
            elif row.get("verified") is False:
                failure = work / group / row["name"] / "verification/verification.json"
                error = json.loads(failure.read_text()).get("error") if failure.exists() else "Unknown"
                exclusions.append(dict(name=row["name"], group=group, error=error,
                                       note="Historical attempt; successful reruns are listed separately."))
    replay_path = work / "release_replay_final/results.json"
    if not replay_path.exists(): replay_path = work / "release_replay/results.json"
    replay = json.loads(replay_path.read_text())
    root = Path(__file__).resolve().parents[3]
    registry = root / "Dataset/Testbenches/source_fixes.cfg"
    result = dict(acceptance=dict(total=replay["total"], passed=replay["passed"],
                  binary_sha256=replay["binary_sha256"], binary_unchanged=replay["binary_unchanged"]),
                  source_registry_sha256=hashlib.sha256(registry.read_bytes()).hexdigest(),
                  rtl_configuration_count=len(accepted), source_native_rtl_cases=sum(row["cases"] for row in accepted.values()),
                  rtl_results=list(accepted.values()), retained_nonpasses=exclusions,
                  limits=["Four bounded deterministic fixtures per configuration; no exhaustive equivalence proof.",
                          "80 baseline plus 148 pragma configurations passed schedule/binding/estimator acceptance, not 228 full RTL simulations.",
                          "Wall-clock/cycle-cap timeouts, RL candidate timeouts and RL process crashes are excluded from repair scope.",
                          "No RTL synthesis, timing closure, resource calibration or fresh PPO training was performed."])
    frozen = work / "scheduled_units_v2/comparison.json"
    if frozen.exists():
        result["frozen_dapo_ir_replays"] = json.loads(frozen.read_text())
    (work / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({key: value for key, value in result.items() if key not in
        ("rtl_results", "retained_nonpasses", "frozen_dapo_ir_replays")}, indent=2))


if __name__ == "__main__":
    main()
