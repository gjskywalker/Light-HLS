#!/usr/bin/env python3
"""Merge completed characterization runs using physical utilization reports."""
import argparse
import json
from pathlib import Path
from characterize import digest, report_resources, IMPLEMENTATION


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runs", type=Path, nargs="+", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    model = dict(schema_version=1, implementation=IMPLEMENTATION, board="u250",
                 part="xcu250-figd2104-2L-e", clock_period_ns=10,
                 scope="isolated_floating_operators_only", status="completed",
                 utilization_basis="physical CLB LUTs / register flip-flops / DSP tiles from report_utilization",
                 entries={}, source_runs=[])
    binary_hashes, emitter_hashes = set(), set()
    for directory in args.runs:
        path = directory / "model.json"
        source = json.loads(path.read_text())
        if source["status"] != "completed" or source["failures"]:
            raise RuntimeError("Characterization incomplete: " + str(directory))
        if source["implementation"] != IMPLEMENTATION or source["part"] != model["part"]:
            raise RuntimeError("Incompatible characterization")
        binary_hashes.add(source["binary_sha256"])
        emitter_hashes.add(source["rtl_emitter_sha256"])
        model["source_runs"].append(dict(path=str(path.resolve()), sha256=digest(path)))
        for key, entry in source["entries"].items():
            entry = dict(entry)
            entry["resources"] = report_resources(entry["report"])
            entry["report_sha256"] = digest(entry["report"])
            if key in model["entries"] and model["entries"][key]["resources"] != entry["resources"]:
                raise RuntimeError("Conflicting measurements: " + key)
            model["entries"][key] = entry
    if len(binary_hashes) != 1 or len(emitter_hashes) != 1:
        raise RuntimeError("Mixed binary or emitter versions")
    model["binary_sha256"] = binary_hashes.pop()
    model["rtl_emitter_sha256"] = emitter_hashes.pop()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.output.exists(): raise RuntimeError("Refusing to overwrite an installed model")
    args.output.write_text(json.dumps(model, indent=2)+"\n")
    print("Installed", len(model["entries"]), "physical-resource entries:", args.output)


if __name__ == "__main__": main()
