"""Measure the physical effect of Bambu's --no-chaining switch on fresh verified RTL."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re

from paper_physical import HERE, command, hash_file, implementation


def comparable_rtl(rtl):
    text = rtl.read_text().replace(str(rtl.parent) + "/", "")
    return re.sub(r"(?m)^(// Code created using PandA - .*? - Date )[^\r\n]+$", r"\1<TIME>", text)


def measure(item, matrix, output, collect_only, reference_matrix):
    case, arm = item
    source = (matrix / case / arm).resolve()
    rtl = source / "main_top.v"
    simulation = json.loads((source / "measurement.json").read_text())
    if simulation["status"] != "valid" or hash_file(rtl) != simulation["rtl_sha256"]:
        raise ValueError(f"{case}/{arm} must have a verified C/RTL checksum for the same RTL")
    text = rtl.read_text()
    files = re.findall(r'\.MEMORY_INIT_file\("([^"]+)"\)', text)
    memories = {}
    for name in files:
        path = Path(name)
        if path.parent.resolve() != source or not path.is_file():
            raise ValueError(f"Unexpected or missing initialized RAM: {path}")
        memories[path.name] = hash_file(path)
    folder = output / case / arm
    if not collect_only:
        folder.mkdir(parents=True, exist_ok=False)
        command(["vivado", "-mode", "batch", "-notrace", "-source", str(HERE / "paper_physical.tcl"),
                 "-tclargs", str(rtl), "main_top", "clock"], folder, "capture.log")
    row = implementation(folder)
    log = (folder / "capture.log").read_text()
    if re.search(r"(could not|failed to|unable to) open.*(?:\$readmem|\.mem\b)", log, re.I):
        raise ValueError(f"A synthesized RAM initialization file was missing in {folder}")
    cycles = simulation["cycles"]
    meets = row["wns_ns"] >= 0 and row["whs_ns"] >= 0
    row.update(rtl_sha256=hash_file(rtl), memory_files=memories, cycles=cycles,
               checksum_hex=simulation["expected_checksum"], meets_100mhz=meets,
               time_us_at_100mhz=cycles / 100 if meets else None,
               estimated_time_us=round(cycles*row["delay_ns"]/1000, 4))
    if reference_matrix:
        reference = (reference_matrix / case / arm).resolve()
        reference_rtl = reference / "main_top.v"
        check = json.loads((reference / "measurement.json").read_text())
        data = comparable_rtl(rtl)
        if (comparable_rtl(reference_rtl) != data or check["status"] != "valid" or
                check["rtl_sha256"] != hash_file(reference_rtl) or check["cycles"] != cycles or
                check["expected_checksum"] != row["checksum_hex"] or
                {p.name:hash_file(p) for p in reference.glob("*.mem")} != memories):
            raise ValueError(f"Physical input differs from the reference matrix for {item}")
        row.update(reference_matrix_rtl_sha256=hash_file(reference_rtl),
                   normalized_rtl_sha256=hashlib.sha256(data.encode()).hexdigest(),
                   reference_input_verified=True)
    print(case, arm, cycles, row["wns_ns"], flush=True)
    return case, arm, row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--matrix", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--summary", type=Path, required=True)
    parser.add_argument("--cases", nargs="+", default=["bicg","jacobi_1d"])
    parser.add_argument("--jobs", default=2, type=int)
    parser.add_argument("--collect-only", action="store_true")
    parser.add_argument("--reference-matrix", type=Path,
                        help="verify cycles, initialized RAMs and RTL after removing source path and build timestamp")
    args = parser.parse_args()
    matrix, output = args.matrix.resolve(), args.output.resolve()
    rows = {name: {} for name in args.cases}
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        tasks = [(name, arm) for name in args.cases for arm in ("default","no_chaining")]
        for name, arm, row in pool.map(lambda item: measure(item, matrix, output, args.collect_only,
                                                            args.reference_matrix), tasks):
            rows[name][arm] = row
    document = {"tcl_sha256":hash_file(HERE/"paper_physical.tcl"),"clock_ns":10,
                "board":"xc7z020clg484-1", "bambu_option_delta":{"default":[],"no_chaining":["--no-chaining"]},
                "timing_measure":"post-route estimate: RTL cycles * (10 ns - WNS); single implementation per design",
                "cases":rows}
    args.summary.write_text(json.dumps(document, indent=2)+"\n")


if __name__ == "__main__":
    main()
