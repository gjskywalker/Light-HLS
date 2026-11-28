"""Resynthesize the Vericert programs with controlled switches in PandA 2023.1."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import tarfile
import tempfile


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
BUNDLE = ROOT / "benchmarks/hls_scheduling_2024_2026"
PAPER = ROOT / "runs/benchmark_collection_20261119/static/vericert-pldi2024"
REVISION = "04336c437a53bc96ae90b74052c455629946ec8b"
FRONT_OFF = ["-fno-if-conversion", "-O0"]
CHAIN_OFF = ["--no-chaining", "--do-not-chain-memories"]
ASYNC_OFF = ["--do-not-use-asynchronous-memories"]
# Numeric metric values follow the implementation of this revision.
ARMS = {
    "default": [],
    "static_mobility": ["--parametric-list-based=2"],
    "fixed_priority": ["--parametric-list-based=1"],
    "single_channel": ["--channels-type=MEM_ACC_11","--channels-number=1"],
    "no_chaining": ["--no-chaining"],
    "no_memory_chaining": ["--do-not-chain-memories"],
    "no_async_memory": ["--do-not-use-asynchronous-memories"],
    "frontend_O0": ["-O0"],
    "no_if_conversion": ["-fno-if-conversion"],
    "O0_no_chaining": ["-O0", "--no-chaining"],
    "paper_noopt": ["-fno-if-conversion", "-O0", "--do-not-chain-memories",
                    "--do-not-use-asynchronous-memories", "--clock-period=10", "--no-chaining"],
    "F": FRONT_OFF,
    "S": CHAIN_OFF,
    "FS": FRONT_OFF + CHAIN_OFF,
    "FM": FRONT_OFF + ASYNC_OFF,
    "SM": CHAIN_OFF + ASYNC_OFF,
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n")


def read_benches(archive):
    benches = {}
    with tarfile.open(archive) as tar:
        for obj in tar:
            if (obj.name.startswith("synthesis-results/bambu-opt/") and "/output/" not in obj.name
                    and obj.name.endswith("/HLS_output/simulation/testbench_main_top.v")):
                name = obj.name.split("/")[-4].replace("-", "_")
                tb = tar.extractfile(obj).read().decode()
                tb, changes = re.subn(r'\$fopen\("[^"]*/(values.txt|results.txt)"',
                                      r'$fopen("\1"', tb)
                if changes != 2 or name in benches:
                    raise ValueError(f"unexpected archived testbench for {name}")
                benches[name] = tb
    return benches


def run(cmd, cwd, env, log, timeout=180):
    with (cwd/log).open("wb") as output:
        subprocess.run(cmd, cwd=cwd, env=env, stdout=output, stderr=subprocess.STDOUT,
                       check=True, timeout=timeout)


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--appdir", type=Path, required=True, help="extracted official 2023.01 AppImage")
    cli.add_argument("--archive", type=Path, default=PAPER/"artefact/synthesis-results.tar.xz")
    cli.add_argument("--run", type=Path, required=True)
    cli.add_argument("--output", type=Path, required=True)
    cli.add_argument("--jobs", type=int, default=5)
    cli.add_argument("--sim-timeout", type=int, default=180)
    cli.add_argument("--retry-simulation", action="store_true",
                     help="resume a stopped run, reusing completed measurements and retrying a timed-out vvp")
    cli.add_argument("--cases", nargs="+", help="case names with underscores, default all 27")
    cli.add_argument("--arms", nargs="+", choices=ARMS, default=list(ARMS))
    args = cli.parse_args()
    dest, app = args.run.resolve(), args.appdir.resolve()
    binary = app/"AppRun"
    dataset = json.loads((HERE/"paper_repaired_results_20261125.json").read_text())
    sources = {r["id"][11:]:BUNDLE/r["source_path"] for r in json.loads((BUNDLE/"cases.json").read_text())
               if r["id"].startswith("vericert24_")}
    names = args.cases or sorted(sources)
    benches = read_benches(args.archive)
    dest.mkdir(parents=True, exist_ok=args.retry_simulation)
    env = dict(os.environ, APPDIR=str(app), ARGV0="bambu")
    # Older bundled Clang targets a 32-bit ABI, using the host's multilib headers.
    env["C_INCLUDE_PATH"] = "/usr/include/x86_64-linux-gnu"
    env["OWD"] = str(dest)
    version = subprocess.check_output([binary, "--version"], cwd=dest, env=env, text=True,
                                      stderr=subprocess.STDOUT)
    if REVISION not in version:
        raise ValueError("this protocol is for the revision in the archived RTL")

    def measure(task):
        n, arm = task
        src = sources[n]
        witness = dataset["protocol"]["source_hashes"]["vericert24_"+n]
        if sha(src) != witness["original"]:
            raise ValueError(f"source changed: {n}")
        folder = dest/n/arm
        previous = None
        measurement = folder/"measurement.json"
        if args.retry_simulation and measurement.exists():
            previous = json.loads(measurement.read_text())
            if previous["status"] == "valid" or "['vvp', 'sim']' timed out" not in previous["diagnostic"]:
                return n,arm,previous
        else:
            if args.retry_simulation and folder.exists():
                # An interrupted compiler may have left a partial binary or RTL file.
                # Retain that work separately and regenerate this arm from its source.
                saved = Path(tempfile.mkdtemp(prefix=f"{dest.name}-{n}-{arm}-", dir=dest.parent))
                folder.rename(saved/"partial")
            folder.mkdir(parents=True)
        c = src.read_text()
        # Match the author's rename, relocate only the common header.
        c, count = re.subn(r"\bmain(?=\s*\()", "main_top", c)
        if count != 1:
            raise ValueError(f"unexpected entry point in {src}")
        for include in re.findall(r'#include\s+"([^"]*misc\.h)"', c):
            shutil.copyfile((src.parent/include).resolve(), folder/"misc.h")
            c = c.replace('"'+include+'"', '"misc.h"')
        if previous:
            if ((folder/src.name).read_text()!=c or (folder/"author_tb.v").read_text()!=benches[n]):
                raise ValueError(f"inputs changed since first attempt: {n}/{arm}")
        else:
            (folder/src.name).write_text(c)
            (folder/"author_tb.v").write_text(benches[n])
        expected = int(witness["checksum"], 16)
        (folder/"values.txt").write_text(f"o{expected:032b}\ne\n")
        params = [*ARMS[arm], "--top-fname=main_top", src.name]
        row = {"status":"running", "expected_checksum":witness["checksum"]}
        try:
            rtl = folder/"main_top.v"
            if not previous:
                run([binary, *params], folder, dict(env, OWD=str(folder)), "compile.log")
                run(["iverilog","-g2012","-s","main_top_tb_top","-o","sim",str(rtl),"author_tb.v"],
                    folder, os.environ, "iverilog.log")
            else:
                row["previous"] = previous
            logname = "rtl_retry.log" if previous else "rtl.log"
            run(["vvp","sim"], folder, os.environ, logname, timeout=args.sim_timeout)
            valid, cycles = (folder/"results.txt").read_text().split()
            log = (folder/logname).read_text()
            match = re.search(r"return_port\s*=\s*(\d+)\s+expected\s*=\s*(\d+)",log)
            if valid != "1" or not match or int(match[1]) != expected or int(match[2]) != expected:
                raise ValueError("RTL return mismatch")
            row.update(status="valid", cycles=int(cycles), rtl_sha256=sha(rtl))
        except (subprocess.SubprocessError, OSError, ValueError) as error:
            row.update(status="failed", diagnostic=str(error))
        save(folder/"measurement.json", row)
        return n,arm,row

    rows = {n:{} for n in names}
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for n,arm,r in pool.map(measure, [(n,k) for n in names for k in args.arms]):
            rows[n][arm] = r
    summary = {}
    if "default" in args.arms:
        for arm in args.arms:
            pairs = [(r[arm]["cycles"],r["default"]["cycles"]) for r in rows.values()
                     if r[arm]["status"] == r["default"]["status"] == "valid"]
            summary[arm] = {"pairs":len(pairs), "lower":sum(x<y for x,y in pairs),
                            "equal":sum(x==y for x,y in pairs), "higher":sum(x>y for x,y in pairs),
                            "geomean_cycles_over_default":statistics.geometric_mean(x/y for x,y in pairs) if pairs else None}
    reproduction = {}
    for arm,column in (("default","bambu-opt"),("paper_noopt","bambu-noopt")):
        if arm in args.arms:
            reproduction[arm] = {n:r[arm].get("cycles")==dataset["cases"]["vericert24_"+n]["published"][column]
                                 for n,r in rows.items()}
    save(args.output, {"revision":REVISION,"bambu_binary_sha256":sha(app/"usr/bin/bambu"),
                       "archive_sha256":sha(args.archive),"flags":{k:ARMS[k] for k in args.arms},
                       "summary":summary,"archive_cycle_match":reproduction,"cases":rows})
    print(json.dumps(summary,indent=2))
    if any(v["status"]!="valid" for r in rows.values() for v in r.values()):
        raise SystemExit("incomplete RTL validation; see case logs")


if __name__ == "__main__":
    main()
