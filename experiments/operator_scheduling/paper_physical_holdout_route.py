"""Route a hash-selected holdout subset using the same Tcl as the development cases."""

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
from pathlib import Path
import re
import tarfile

from paper_physical import ARCHIVE, hash_file, implementation, measure
from paper_physical_deployed import read, selected


SALT = "Light-HLS-holdout-route-v1/"
SHA = "f43b0201b64b23ac071ae54af317984a40fb067943f1e09ccd22bd17c0e100f5"


def freeze(frozen):
    p = read(frozen / "protocol.json")
    result = []
    for case in p["holdout"]:
        row = read(frozen / "native" / case / "experiment.json")
        baseline = row["native"]["status"]
        deployed = row.get("agent", {}).get("deployed", {}).get("status")
        result.append({"case": case, "sha256": hashlib.sha256((SALT + case).encode("ascii")).hexdigest(),
                       "native_status": baseline, "single_deployed_status": deployed,
                       "eligible": baseline == deployed == "valid"})
    return sorted(result, key=lambda x: x["sha256"])


def prepare_bambu(run, names, archive):
    if hash_file(archive) != SHA:
        raise ValueError("PLDI 2024 archive SHA-256 changed")
    with tarfile.open(archive, "r:xz") as tar:
        members = [m for m in tar.getmembers() if m.isfile()
                   and m.name.startswith("synthesis-results/bambu-opt/")
                   and not m.name.startswith("synthesis-results/bambu-opt/output/")]
        for name in names:
            folder = run / "bambu" / name
            folder.mkdir(parents=True, exist_ok=True)
            for filename in ("main_top.v", "testbench_main_top.v"):
                member, = [m for m in members if "/" + name.replace("_", "-") + "/" in m.name
                           and m.name.endswith("/" + filename)]
                (folder / filename).write_bytes(tar.extractfile(member).read())


def physical(folder, rtl):
    row = implementation(folder)
    timing = dict(line.split("\t", 1) for line in (folder / "timing.tsv").read_text().splitlines())
    row.update(setup_start_pin=timing["start_pin"], setup_end_pin=timing["end_pin"],
               meets_100mhz=row["wns_ns"] >= 0 and row["whs_ns"] >= 0)
    row["rtl_sha256"] = hash_file(rtl)
    if (folder / "rtl.sha256").read_text().strip() != row["rtl_sha256"]:
        raise ValueError(f"RTL changed after implementation in {folder}")
    return row


def measure_case(job, frozen, run, collect_only):
    case, kind = job
    name = case.removeprefix("vericert24_")
    original = read(frozen / "native" / case / "experiment.json")
    if kind == "bambu":
        root = run
        rtl_kind = "bambu"
        rtl = root / "bambu" / name / "main_top.v"
        record = {"cycles": int(original["published"]["bambu-opt"]), "checksum": original["native"]["checksum"]}
        choice = "bambu-opt"
    else:
        root = run / kind
        rtl_kind = "light"
        if kind == "native":
            rtl = frozen / "native" / case / "hls/reports/rtl/top.v"
            record, choice = original["native"], "native"
        else:
            record, choice, rtl = selected(frozen, case, "manual" if kind == "manual" else "agent")
        link = root / "inputs" / case / "hls/reports/rtl/top.v"
        if not collect_only:
            link.parent.mkdir(parents=True, exist_ok=True)
            if not link.exists():
                link.symlink_to(rtl)
        if hash_file(link) != hash_file(rtl):
            raise ValueError(f"Input RTL mismatch: {case} {kind}")
    folder = root / rtl_kind / name
    if not collect_only and not (folder / "timing.tsv").is_file():
        measure(root / "inputs", root, rtl_kind, name, None, None)
    out = physical(folder, rtl)
    sim = (folder / "simulation.log").read_text()
    if kind == "bambu":
        passed, cycles = (folder / "results.txt").read_text().split()
        m = re.search(r"return_port\s*=\s*(\d+)\s+expected\s*=\s*(\d+)", sim)
        if passed != "1" or not m or int(m[1]) != int(m[2]):
            raise ValueError(f"Bambu RTL disagrees with original C: {name}")
        checksum = f"{int(m[1]):08x}"
        out["missing_memory_init_files"] = sorted(set(
            re.findall(r"\$readmemb: Unable to open .*/([^/ ]+\.mem) for reading", sim)))
    else:
        cycles, checksum = re.search(r"cycles=(\d+) checksum=([0-9a-fA-F]{8})", sim).groups()
        out["frozen_rtl"] = str(rtl.relative_to(frozen))
    if int(cycles) != record["cycles"] or checksum != record["checksum"]:
        raise ValueError(f"Measured cycles/checksum disagree with reference: {case} {kind}")
    out.update(choice=choice, cycles=int(cycles), checksum_hex=checksum,
               execution_us_at_100mhz=int(cycles) / 100 if out["meets_100mhz"] else None,
               execution_us_at_design_delay=round(int(cycles) * out["delay_ns"] / 1000, 4))
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--frozen", required=True, type=Path)
    parser.add_argument("--run", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--archive", type=Path, default=ARCHIVE)
    parser.add_argument("--collect-only", action="store_true")
    parser.add_argument("--with-manual", action="store_true", help="also compare the frozen manual policy")
    args = parser.parse_args()
    frozen, run = args.frozen.resolve(), args.run.resolve()
    run.mkdir(parents=True, exist_ok=True)
    ranked = freeze(frozen)
    chosen = [r["case"] for r in ranked if r["eligible"]][:4]
    if len(ranked) != 19 or len(chosen) != 4:
        raise ValueError("Unexpected frozen holdout population")
    result = {"salt": SALT, "ranking": ranked, "chosen": chosen,
              "cases": {case: {} for case in chosen}}
    jobs = [(case, kind) for case in chosen for kind in ("deployed", "bambu")]
    manual_reuse = {}
    for case in chosen:
        _, _, agent_rtl = selected(frozen, case, "agent")
        native_rtl = frozen / "native" / case / "hls/reports/rtl/top.v"
        if hash_file(agent_rtl) != hash_file(native_rtl):
            jobs.append((case, "native"))
        if args.with_manual:
            _, _, manual_rtl = selected(frozen, case, "manual")
            for label, path in (("deployed", agent_rtl), ("native", native_rtl)):
                if hash_file(path) == hash_file(manual_rtl):
                    manual_reuse[case] = label
                    break
            else:
                jobs.append((case, "manual"))
    if not args.collect_only:
        (run / "selection.json").write_text(json.dumps(result, indent=2) + "\n")
        prepare_bambu(run, [s.removeprefix("vericert24_") for s in chosen], args.archive)
    with ThreadPoolExecutor(max_workers=2) as pool:
        queued = {pool.submit(measure_case, job, frozen, run, args.collect_only): job for job in jobs}
        for task in as_completed(queued):
            case, kind = queued[task]
            result["cases"][case][kind] = task.result()
            print(f"{case} {kind} complete", flush=True)
    for case, rows in result["cases"].items():
        if "native" not in rows:
            native = read(frozen / "native" / case / "experiment.json")["native"]
            if (rows["deployed"]["cycles"], rows["deployed"]["checksum_hex"]) != (native["cycles"], native["checksum"]):
                raise ValueError(f"Native verification differs for an identical netlist: {case}")
            rows["native"] = dict(rows["deployed"], choice="native", reused_identical_deployed_rtl=True)
        if case in manual_reuse:
            record, choice, rtl = selected(frozen, case, "manual")
            matching = rows[manual_reuse[case]]
            if (matching["cycles"], matching["checksum_hex"]) != (record["cycles"], record["checksum"]):
                raise ValueError(f"Manual verification differs for an identical netlist: {case}")
            rows["manual"] = dict(matching, choice=choice, frozen_rtl=str(rtl.relative_to(frozen)),
                                  reused_identical_rtl_from=manual_reuse[case])
    result["actual_implementations"] = len(jobs)
    args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")


if __name__ == "__main__":
    main()
