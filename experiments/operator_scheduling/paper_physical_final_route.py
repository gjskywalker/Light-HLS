"""Verify and route the final deployed RTL for the seven previously sampled programs."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import re

from paper_physical import CASES, ROOT, hash_file, measure
from paper_physical_deployed import read, selected
from paper_physical_holdout_route import physical


DEV = CASES
HOLDOUT = ("fdtd_2d", "syr2k", "trmm", "cholesky")
PREVIOUS = ("paper_physical_20261125", "paper_physical_deployed_20261125", "paper_physical_holdout_20261125")


def final_rtl(single, joint, case, role, record):
    final = record[role]
    if final["status"] != "valid":
        raise ValueError(f"Invalid final policy: {case}/{role}")
    if final["source"] == "single":
        base, choice, rtl = selected(single, case, role)
        if base["cycles"] != final["cycles"]:
            raise ValueError(f"Frozen and final single cycles differ: {case}/{role}")
        return rtl, choice
    if final["source"] != "joint":
        raise ValueError(f"Unknown deployment source: {case}/{role}")
    expected = sorted(final["positive"])
    for folder in (joint / case / role, joint / case / "manual"):
        schedule = folder / "hls/reports/scheduling/schedule.json"
        cfg = folder / "joint.cfg"
        if not (schedule.is_file() and cfg.is_file()):
            continue
        labels = [p["loop"] for p in read(schedule)["pipelines"]]
        configured = re.findall(r"^loop_pipeline label=(\S+) II=1$", cfg.read_text(), re.M)
        if sorted(labels) == sorted(configured) == expected:
            return folder / "hls/reports/rtl/top.v", expected
    raise ValueError(f"No implemented joint matches {case}/{role}: {expected}")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--single", type=Path, required=True)
    p.add_argument("--joint", type=Path, required=True)
    p.add_argument("--run", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--prior-runs", type=Path, default=ROOT / "runs")
    p.add_argument("--collect-only", action="store_true")
    args = p.parse_args()
    single, joint, run = (v.resolve() for v in (args.single, args.joint, args.run))
    prior = args.prior_runs.resolve()
    archive = read(joint / "results.json")
    if archive["parent_run"] != hash_file(single / "results.json"):
        raise ValueError("Joint and single frozen result sets differ")
    previous = {}
    for source in PREVIOUS:
        for h in (prior / source).rglob("rtl.sha256"):
            if (h.parent / "timing.tsv").is_file() and (h.parent / "simulation.log").is_file():
                previous[h.read_text().strip()] = h.parent
    run.mkdir(parents=True, exist_ok=True)

    def process(name):
        case = "vericert24_" + name
        base = read(single / "native" / case / "experiment.json")
        record = archive["cases"][case]
        rtl, choice = final_rtl(single, joint, case, "agent", record)
        manual_rtl, manual_choice = final_rtl(single, joint, case, "manual", record)
        sha = hash_file(rtl)
        folder = previous.get(sha)
        reused = folder is not None
        if not reused:
            folder = run / "light" / name
            link = run / "inputs" / case / "hls/reports/rtl/top.v"
            if not args.collect_only:
                link.parent.mkdir(parents=True, exist_ok=True)
                if not link.exists():
                    link.symlink_to(rtl)
                if hash_file(link) != sha:
                    raise ValueError(f"Input RTL changed: {name}")
                if not (folder / "timing.tsv").is_file():
                    measure(run / "inputs", run, "light", name, None, None)
        row = physical(folder, rtl)
        cycles, checksum = re.search(r"cycles=(\d+) checksum=([a-fA-F0-9]{8})",
                                     (folder / "simulation.log").read_text()).groups()
        cycles = int(cycles)
        if cycles != record["agent"]["cycles"] or checksum != base["native"]["checksum"]:
            raise ValueError(f"Final and routed C/RTL observations differ: {case}")
        row.update(cycles=cycles, checksum_hex=checksum, choice=choice, source=record["agent"]["source"],
                   rtl=rtl.relative_to((single if record["agent"]["source"] == "single" else joint)).as_posix(),
                   previous_physical_reused=reused)
        row["physical_directory"] = str(folder.relative_to(prior) if reused else Path("new") / "light" / name)
        row["execution_us_at_design_delay"] = round(cycles * row["delay_ns"] / 1000, 4)
        row["execution_us_at_100mhz"] = cycles / 100 if row["meets_100mhz"] else None
        bambu_folder = prior / PREVIOUS[0 if name in DEV else 2] / "bambu" / name
        bambu = physical(bambu_folder, bambu_folder / "main_top.v")
        passed, count = (bambu_folder / "results.txt").read_text().split()
        match = re.search(r"return_port\s*=\s*(\d+)\s+expected\s*=\s*(\d+)",
                          (bambu_folder / "simulation.log").read_text())
        if (passed != "1" or int(count) != int(base["published"]["bambu-opt"]) or not match or
                int(match[1]) != int(match[2]) or int(match[1]) != int(checksum, 16)):
            raise ValueError(f"Published Bambu and routed RTL observations differ: {case}")
        bambu.update(cycles=int(count), checksum_hex=checksum,
                     physical_directory=str(bambu_folder.relative_to(prior)),
                     execution_us_at_design_delay=round(int(count) * bambu["delay_ns"] / 1000, 4),
                     execution_us_at_100mhz=int(count) / 100 if bambu["meets_100mhz"] else None)
        manual_hash = hash_file(manual_rtl)
        return {"split": "development" if name in DEV else "holdout", "agent": row, "bambu": bambu,
                "manual": {"choice": manual_choice, "rtl_sha256": manual_hash,
                           "cycles": record["manual"]["cycles"], "same_rtl_as_agent": sha == manual_hash}}

    with ThreadPoolExecutor(max_workers=2) as pool:
        rows = dict(zip(DEV + HOLDOUT, pool.map(process, DEV + HOLDOUT)))
    result = {"compiler_sha256": read(single / "protocol.json")["compiler_sha256"],
              "parent_single_sha256": archive["parent_run"], "joint_results_sha256": hash_file(joint / "results.json"),
              "cases": rows}
    args.output.write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
