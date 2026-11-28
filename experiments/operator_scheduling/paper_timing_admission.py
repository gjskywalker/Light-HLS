"""Select a verified implementation for fixed-frequency operation from routed Light-HLS candidates."""
import argparse
import json
import math
from pathlib import Path


def meets_target(row, period):
    return (row["routed_nets"] > 0 and row["wns_ns"] >= 0 and row["whs_ns"] >= 0
            and math.isclose(row["delay_ns"] + row["wns_ns"], period, abs_tol=0.002))


def select(native, candidate, period):
    if (native["vivado"], native["part"], native["checksum_hex"]) != (
            candidate["vivado"], candidate["part"], candidate["checksum_hex"]):
        raise ValueError("candidate and baseline must use the same flow, part and C oracle")
    alternatives = {"native": native, "agent": candidate}
    valid = [name for name, row in alternatives.items() if meets_target(row, period)]
    chosen = min(valid, key=lambda name: alternatives[name]["cycles"], default=None)
    return {"passes": valid, "selected": chosen,
            "cycles": alternatives[chosen]["cycles"] if chosen else None,
            "time_us_at_target": alternatives[chosen]["cycles"] * period / 1000 if chosen else None,
            "sha256": alternatives[chosen]["rtl_sha256"] if chosen else None}


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    for name in ("native", "deployed", "holdout", "output"):
        cli.add_argument("--" + name, type=Path, required=True)
    args = cli.parse_args()
    dev = json.loads(args.native.read_text())
    deployed = json.loads(args.deployed.read_text())
    holdout = json.loads(args.holdout.read_text())["cases"]
    rows = {n: (r["light"], deployed[n], r["bambu"]) for n, r in dev.items()}
    rows.update({n: (r["native"], r["deployed"], r["bambu"]) for n, r in holdout.items()})
    results = {}
    for name, (native, candidate, bambu) in rows.items():
        result = select(native, candidate, 10)
        result["split"] = "dev" if name in dev else "holdout"
        result["bambu_cycles_at_target"] = bambu["cycles"] if meets_target(bambu, 10) else None
        results[name] = result
    args.output.write_text(json.dumps({"period_ns": 10, "cases": results}, indent=2) + "\n")


if __name__ == "__main__":
    main()
