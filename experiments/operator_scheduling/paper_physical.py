"""Reproduce three same-part, 10 ns OOC measurements for the 2024 Vericert comparison."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import csv
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
import tarfile
from xml.etree import ElementTree


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
COLLECTION = ROOT / "benchmarks/hls_scheduling_2024_2026/sources/vericert_pldi2024_archive/benchmarks/polybench-syn"
ARCHIVE = ROOT / "runs/benchmark_collection_20261119/static/vericert-pldi2024/artefact/synthesis-results.tar.xz"
CASES = ("gemm", "bicg", "jacobi_1d")
PAPER_CONFIGS = ("vericert-hyperblock", "vericert-list-scheduling", "vericert-original",
                 "bambu-opt", "bambu-noopt")


def hash_file(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def command(argv, directory, log):
    with (directory / log).open("w") as handle:
        subprocess.run(argv, cwd=directory, stdout=handle, stderr=subprocess.STDOUT, check=True)


def prepare_archive(directory, archive):
    if hash_file(archive) != "f43b0201b64b23ac071ae54af317984a40fb067943f1e09ccd22bd17c0e100f5":
        raise ValueError("Unexpected PLDI 2024 synthesis archive")
    with tarfile.open(archive, "r:xz") as bundle:
        archived = {case: {} for case in CASES}
        for config in PAPER_CONFIGS:
            prefix = "synthesis-results/" + config + "/"
            files = [m for m in bundle.getmembers() if m.name.startswith(prefix)
                     and not m.name.startswith(prefix + "output/") and m.isfile()]
            members = {Path(m.name).name: m for m in files}
            cycles = dict(csv.reader(io.TextIOWrapper(bundle.extractfile(members["exec.csv"]))))
            for case in CASES:
                name = case.replace("_", "-")
                report = bundle.extractfile(members[name + "_report.xml"]).read()
                log = bundle.extractfile(members[name + "_synth.log"]).read().decode()
                paper = archive_metrics(report)
                paper.update(cycles=int(cycles[name]), vivado=re.search(r"Vivado v([\d.]+)", log)[1],
                             wns_ns=round(10 - paper["delay_ns"], 3))
                archived[case][config] = paper
                if config == "vericert-hyperblock":
                    folder = directory / "vericert" / case
                    folder.mkdir(parents=True, exist_ok=True)
                    (folder / "author.sv").write_bytes(bundle.extractfile(members[name + ".sv"]).read())
                if config == "bambu-opt":
                    folder = directory / "bambu" / case
                    folder.mkdir(parents=True, exist_ok=True)
                    for filename in ("main_top.v", "testbench_main_top.v"):
                        match, = [m for m in files if "/" + name + "/" in m.name and m.name.endswith("/" + filename)]
                        (folder / filename).write_bytes(bundle.extractfile(match).read())
    return archived


def source_and_top(native, folder, kind, case):
    if kind == "vericert":
        return folder / "author.sv", "main", "clk"
    if kind == "bambu":
        return folder / "main_top.v", "main_top", "clock"
    return native / ("vericert24_" + case) / "hls/reports/rtl/top.v", "benchmark_main", "ap_clk"


def measure(native, run, kind, case, verilator, driver):
    folder = run / kind / case
    folder.mkdir(parents=True, exist_ok=True)
    rtl, top, clock = source_and_top(native, folder, kind, case)
    source = next(COLLECTION.rglob(case.replace("_", "-") + ".c"))
    command(["cc", "-O0", str(source), "-o", "oracle"], folder, "oracle_build.log")
    oracle = subprocess.run(["./oracle"], cwd=folder, text=True, capture_output=True)
    (folder / "oracle.log").write_text(oracle.stdout)
    expected = int(re.search(r"^finished:? (\d+)$", oracle.stdout, re.M)[1])
    if kind == "bambu":
        testbench, n = re.subn(r'\$fopen\("[^"]*/(values.txt|results.txt)"', r'$fopen("\1"',
                               (folder / "testbench_main_top.v").read_text())
        if n != 2:
            raise ValueError("Unexpected filenames in the Bambu testbench")
        (folder / "relocated_tb.v").write_text(testbench)
        (folder / "values.txt").write_text(f"o{expected:032b}\ne\n")
        command(["iverilog", "-g2012", "-s", "main_top_tb_top", "-o", "sim", str(rtl), "relocated_tb.v"],
                folder, "sim_build.log")
        command(["vvp", "sim"], folder, "simulation.log")
        if (folder / "results.txt").read_text().split()[0] != "1":
            raise ValueError("Bambu C/RTL checksum mismatch")
    else:
        defines = ["-DLIGHT_HLS"] if kind == "light" else []
        command(["iverilog", "-g2012", *defines, "-s", "physical_tb", "-o", "sim", str(rtl),
                 str(HERE / "paper_physical_tb.v")], folder, "sim_build.log")
        command(["vvp", "sim", f"+expected={expected:08x}"], folder, "simulation.log")
    if kind == "vericert" and verilator:
        command([verilator, "--cc", "--exe", "-DSYNTHESIS", "-Wno-fatal", "-Wno-lint",
                 "-Wno-style", "-Wno-WIDTH", "--top-module", "main", str(rtl), str(driver)],
                folder, "verilator_translate.log")
        command(["make", "-C", "obj_dir", "-j2", "-f", "Vmain.mk"], folder, "verilator_build.log")
        command([str(folder / "obj_dir/Vmain")], folder, "verilator_run.log")
    (folder / "rtl.sha256").write_text(hash_file(rtl) + "\n")
    command(["vivado", "-mode", "batch", "-notrace", "-source", str(HERE / "paper_physical.tcl"),
             "-tclargs", str(rtl), top, clock], folder, "capture.log")


def archive_metrics(report):
    items = {n.get("stringID"): n.get("value") for n in
             ElementTree.fromstring(report).iter("item")}
    return {key: float(items["XILINX_" + label]) for key, label in
            (("lut", "SLICE_LUTS"), ("ff", "SLICE_REGISTERS"), ("slice", "SLICE"),
             ("dsp", "DSPS"), ("bram18_equiv", "BLOCK_RAMFIFO"), ("delay_ns", "DESIGN_DELAY"))}


def implementation(folder):
    timing = dict(csv.reader((folder / "timing.tsv").read_text().splitlines(), delimiter="\t"))
    util = (folder / "post_route_util.rpt").read_text()
    route = (folder / "post_route_status.rpt").read_text()
    errors = int(re.search(r"# of nets with routing errors[. ]+:\s*(\d+)", route)[1])
    full = int(re.search(r"# of fully routed nets[. ]+:\s*(\d+)", route)[1])
    routable = int(re.search(r"# of routable nets[. ]+:\s*(\d+)", route)[1])
    if errors or not full or full != routable:
        raise ValueError("Incomplete routing: " + str(folder))

    def count(label):
        return float(re.search(r"^\|\s*" + label + r"\s*\|\s*([\d,.]+)", util, re.M)[1].replace(",", ""))

    slack = float(timing["wns_ns"])
    return {"vivado": timing["vivado"], "part": timing["part"], "wns_ns": slack,
            "whs_ns": float(timing["whs_ns"]), "delay_ns": round(10 - slack, 3),
            "lut": int(count(r"Slice LUTs\*?")), "ff": int(count("Slice Registers")),
            "slice": int(count("Slice")), "bram18_equiv": int(2 * count("Block RAM Tile")),
            "dsp": int(count("DSPs")), "routed_nets": full}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", required=True, type=Path, help="contains vericert24_CASE/hls/... for the Z020 build")
    parser.add_argument("--run", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--archive", type=Path, default=ARCHIVE)
    parser.add_argument("--verilator", help="optional Verilator executable for the author's additional driver")
    parser.add_argument("--driver", type=Path, default=ARCHIVE.parents[1] / "scripts/verilator_main.cpp")
    parser.add_argument("--collect-only", action="store_true", help="read existing implementation reports")
    args = parser.parse_args()
    run, native = args.run.resolve(), args.native.resolve()
    if not args.collect_only:
        run.mkdir(parents=True, exist_ok=False)
    archived = prepare_archive(run, args.archive)
    if not args.collect_only:
        with ThreadPoolExecutor(max_workers=2) as pool:
            futures = [pool.submit(measure, native, run, kind, case, args.verilator, args.driver.resolve())
                       for case in CASES for kind in ("light", "vericert", "bambu")]
            for future in futures:
                future.result()
    result = {}
    for case in CASES:
        rows = {"published_" + config: row for config, row in archived[case].items()}
        for kind in ("light", "vericert", "bambu"):
            folder = run / kind / case
            row = implementation(folder)
            rtl, _, _ = source_and_top(native, folder, kind, case)
            row["rtl_sha256"] = hash_file(rtl)
            if (folder / "rtl.sha256").exists() and (folder / "rtl.sha256").read_text().strip() != row["rtl_sha256"]:
                raise ValueError(f"RTL changed after implementation: {case} {kind}")
            sim_log = (folder / "simulation.log").read_text()
            if kind == "bambu":
                passed, cycles = (folder / "results.txt").read_text().split()
                m = re.search(r"return_port\s*=\s*(\d+)\s+expected\s*=\s*(\d+)", sim_log)
                if passed != "1" or not m or int(m[1]) != int(m[2]):
                    raise ValueError("Bambu C/RTL checksum mismatch")
                row["cycles"], row["checksum_hex"] = int(cycles), f"{int(m[1]):08x}"
                row["missing_memory_init_files"] = sorted(set(
                    re.findall(r"\$readmemb: Unable to open .*/([^/ ]+\.mem) for reading", sim_log)))
            else:
                m = re.search(r"cycles=(\d+) checksum=([0-9a-fA-F]{8})", sim_log)
                row["cycles"], row["checksum_hex"] = int(m[1]), m[2]
            cross_check = folder / "verilator_run.log"
            if kind == "vericert" and cross_check.is_file():
                match = re.search(r"cycles: (\d+)\s+finished: (\d+)", cross_check.read_text())
                if not match or (int(match[1]), int(match[2])) != (row["cycles"], int(row["checksum_hex"], 16)):
                    raise ValueError("Verilator and Icarus disagree")
                row["verilator_driver_confirmed"] = True
            rows[kind] = row
        for row in rows.values():
            row["execution_us_at_design_delay"] = round(row["cycles"] * row["delay_ns"] / 1000, 4)
        result[case] = rows
    args.output.write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n")


if __name__ == "__main__":
    main()
