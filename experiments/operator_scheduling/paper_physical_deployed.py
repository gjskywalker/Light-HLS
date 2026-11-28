"""Measure the actual deployed policy from the frozen complete-program run."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import re

from paper_physical import CASES, HERE, hash_file, implementation, measure


def read(path):
    return json.loads(path.read_text())


def selected(root, case, role):
    record = read(root / "native" / case / "experiment.json")[role]["deployed"]
    if record["status"] != "valid":
        raise ValueError(f"{case} {role} did not pass RTL verification")
    choice = record.get("choice", record.get("selection"))
    if choice == "native":
        folder = root / "native" / case
    elif choice and choice.startswith("Loop_"):
        folder = root / "probes" / case / choice
    else:
        folder = root / (role + "_order") / case
    return record, choice, folder / "hls/reports/rtl/top.v"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--frozen", required=True, type=Path)
    parser.add_argument("--run", required=True, type=Path)
    parser.add_argument("--reference", type=Path, default=HERE / "paper_physical_results_20261125.json")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--collect-only", action="store_true")
    args = parser.parse_args()
    frozen, run = args.frozen.resolve(), args.run.resolve()
    reference = read(args.reference)
    if not args.collect_only:
        run.mkdir(parents=True, exist_ok=False)

    def execute(name):
        case = "vericert24_" + name
        record, choice, rtl = selected(frozen, case, "agent")
        manual, manual_choice, manual_rtl = selected(frozen, case, "manual")
        ref = reference[name]["light"]
        sha = hash_file(rtl)
        if sha == ref["rtl_sha256"]:
            physical = dict(ref)
            if record["cycles"] != ref["cycles"] or record["checksum"] != ref["checksum_hex"]:
                raise ValueError("Native hash matches but cycles or checksum changed")
        else:
            folder = run / "light" / name
            target = run / "inputs" / case / "hls/reports/rtl/top.v"
            if not args.collect_only:
                target.parent.mkdir(parents=True)
                target.symlink_to(rtl)
                measure(run / "inputs", run, "light", name, None, None)
            if hash_file(target) != sha or (folder / "rtl.sha256").read_text().strip() != sha:
                raise ValueError("Measured netlist hash does not match final policy")
            cycles, checksum = re.search(r"cycles=(\d+) checksum=([0-9a-fA-F]{8})",
                                         (folder / "simulation.log").read_text()).groups()
            if int(cycles) != record["cycles"] or checksum != record["checksum"]:
                raise ValueError("Independent bench disagrees with original data")
            physical = implementation(folder)
            physical.update(cycles=int(cycles), checksum_hex=checksum, rtl_sha256=sha)
            physical["execution_us_at_design_delay"] = round(int(cycles) * physical["delay_ns"] / 1000, 4)
        return {**physical, "agent_choice": choice, "loop_estimate": record.get("attempts", {}).get(choice),
                "manual_choice": manual_choice,
                "manual_cycles": manual["cycles"], "manual_rtl_identical": hash_file(manual_rtl) == sha,
                "native_rtl_identical": sha == ref["rtl_sha256"],
                "actual_rtl": str(rtl.relative_to(frozen)),
                "bambu_same_version_us": reference[name]["bambu"]["execution_us_at_design_delay"],
                "vericert_same_version_us": reference[name]["vericert"]["execution_us_at_design_delay"]}

    with ThreadPoolExecutor(max_workers=2) as pool:
        measured = dict(zip(CASES, pool.map(execute, CASES)))
    args.output.write_text(json.dumps(measured, indent=2) + "\n")


if __name__ == "__main__":
    main()
