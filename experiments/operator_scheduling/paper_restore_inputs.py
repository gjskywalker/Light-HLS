"""Restore the published split, original input adapters and frozen policies without any model calls."""
import argparse
from pathlib import Path

from flow_pilot import read_json, write_json
from paper_order_flow import COLLECTION, adapter


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("archive", type=Path)
    cli.add_argument("--output", required=True, type=Path)
    args = cli.parse_args()
    data = read_json(args.archive)
    config = data["protocol"]
    root = args.output.resolve()
    prior, loops = root/"order", root/"loops"
    root.mkdir(parents=True, exist_ok=False)
    prior.mkdir()
    loops.mkdir()
    cases = {c["id"]:c for c in read_json(COLLECTION/"cases.json")}
    inputs = {n:adapter(cases[n],prior) for n in config["dev"] + config["holdout"]}
    if inputs != config["source_hashes"]:
        raise ValueError("regenerated source adapters or native checksums differ from the recorded experiment")
    write_json(prior/"protocol.json", {"inputs": inputs, **{k:config[k] for k in
               ("part","period_ns","board_sha256","dev","holdout")}})
    write_json(prior/"frozen.json", data["order_search"])
    write_json(loops/"frozen.json", data["loop_freeze"])


if __name__ == "__main__":
    main()
