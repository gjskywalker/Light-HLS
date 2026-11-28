"""Run the archived Stream-HLS C++ tests, check the tensors and challenge each output."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import math
import os
from pathlib import Path
import re
import resource
import signal
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[2]
COLLECTION = ROOT / "benchmarks/hls_scheduling_2024_2026"
BENCH_READ = re.compile(r"(\w+)ifs\.read\(\(char\*\)(v\d+), (\d+) \* sizeof\(float\)\);")
LOAD_PAIR = re.compile(r"float v\d+ = (v\d+)\[[^;]+;\s*float v\d+ = (v\d+)\[[^;]+;")


def arch_inputs(case, index):
    paths = [p for p in case["bundle_files"] if p.endswith((".cpp", ".bin"))]
    for path in paths:
        data = (COLLECTION / path).read_bytes()
        if hashlib.sha256(data).hexdigest() != index[path]["sha256"]:
            raise ValueError(f"Digest mismatch for {path}")
    kernel = COLLECTION / case["source_path"]
    tb = next(COLLECTION / p for p in paths if p.endswith("_tb.cpp"))
    bins = {Path(p).stem: COLLECTION / p for p in paths if p.endswith(".bin")}
    return kernel, tb, bins


def instrument(source, bins):
    reads = BENCH_READ.findall(source)
    if {name for name, _, _ in reads} != set(bins):
        raise ValueError("Mismatch in archived binaries and stream reads")
    for name, _, length in reads:
        if bins[name].stat().st_size != int(length) * 4:
            raise ValueError(f"Truncated data file {name}")
    if "assert(" not in source or set(re.findall(r" > (\d+\.\d+);", source)) != {"0.100000"}:
        raise ValueError("Unexpected testbench oracle")
    output_vars = {
        expected: actual for actual, expected in LOAD_PAIR.findall(re.sub(r"//[^\n]*", "", source))
    }
    dumps = []
    for name, var, _ in reads:
        if name.startswith("output_"):
            actual = output_vars[var]
            dumps.append(
                f'  std::ofstream("actual_{name}.bin", std::ios::binary).write('
                f"reinterpret_cast<const char*>({actual}), sizeof({actual}));"
            )
    rewritten, count = re.subn(
        r"^  forward\([^;]+\);[^\n]*$",
        lambda m: m.group() + "\n" + "\n".join(dumps),
        source,
        flags=re.MULTILINE,
    )
    if count != 1 or not dumps:
        raise ValueError("Output capture could not match a unique forward call")
    return rewritten


def invoke(argv, work, stage, env=None):
    argv = list(map(str, argv))
    (work / f"{stage}.args.json").write_text(json.dumps(argv, indent=2) + "\n")
    log = work / f"{stage}.log"
    with log.open("w") as out:
        try:
            p = subprocess.run(argv, cwd=work, env=env, stdout=out, stderr=subprocess.STDOUT, timeout=180)
        except subprocess.TimeoutExpired:
            return {"stage": stage, "timeout_seconds": 180}
    return {"stage": stage, "returncode": p.returncode}


def compare(a, b):
    expected, actual = a.read_bytes(), b.read_bytes()
    if len(expected) != len(actual):
        raise ValueError(f"Output byte count mismatch for {a.name}")
    nonfinite = over = 0
    max_error = 0.0
    for (x,), (y,) in zip(struct.iter_unpack("<f", expected), struct.iter_unpack("<f", actual)):
        if not (math.isfinite(x) and math.isfinite(y)):
            nonfinite += 1
            continue
        delta = abs(x - y)
        over += int(delta > 0.1)
        max_error = max(max_error, delta)
    return {
        "elements": len(actual) // 4,
        "nonfinite_pairs": nonfinite,
        "above_absolute_tolerance": over,
        "max_absolute_error": max_error,
    }


def run_case(case, index, opt):
    work = opt.output / case["id"]
    work.mkdir(parents=True, exist_ok=True)
    result = {
        "id": case["id"],
        "status": "incomplete",
        "stages": [],
        "outputs": {},
        "negative_controls": {},
    }
    src, tb, bins = arch_inputs(case, index)
    audited = work / "tb_capture.cpp"
    audited.write_text(instrument(tb.read_text(), bins))
    build = ["g++", "-std=c++17", "-O1", "-ffp-contract=off", "-pthread", "-I" + str(opt.vitis_include)]
    env = dict(os.environ, PRJ_PATH=str(tb.parents[1]))
    build_commands = [
        ("kernel", [*build, "-c", src, "-o", "kernel.o"]),
        ("author_build", [*build, tb, "kernel.o", "-o", "author"]),
        ("audit_build", [*build, audited, "kernel.o", "-o", "audit"]),
    ]
    for stage, argv in build_commands + [("author_run", ["./author"]), ("audit_run", ["./audit"])]:
        step = invoke(argv, work, stage, env)
        result["stages"].append(step)
        if step.get("returncode") != 0 and stage not in ("author_run", "audit_run"):
            result["status"] = "compile_failed"
            return result
    for name, golden in bins.items():
        if not name.startswith("output_"):
            continue
        actual = work / f"actual_{name}.bin"
        if actual.is_file():
            result["outputs"][name] = compare(golden, actual)
        # Isolate the challenged golden. Every other file remains an immutable input symlink.
        data_dir = work / f"challenge_{name}" / "data"
        data_dir.mkdir(parents=True, exist_ok=True)
        for other_name, arch_file in bins.items():
            target = data_dir / arch_file.name
            if other_name == name:
                if target.is_symlink():
                    raise ValueError(f"Unsafe golden challenge destination {target}")
                contents = bytearray(arch_file.read_bytes())
                val = struct.unpack_from("<f", contents)[0]
                struct.pack_into("<f", contents, 0, 0.0 if abs(val) > 1.0 else 100.0)
                target.write_bytes(contents)
            elif not target.exists():
                target.symlink_to(arch_file)
        stage = "challenge_" + name
        step = invoke(["./author"], work, stage, dict(env, PRJ_PATH=str(data_dir.parent)))
        diagnostic = (work / f"{stage}.log").read_text(errors="replace")
        result["negative_controls"][name] = (
            step.get("returncode") == -signal.SIGABRT and "Error!" in diagnostic
        )
    all_names = {n for n in bins if n.startswith("output_")}
    good = (
        all(s.get("returncode") == 0 for s in result["stages"])
        and set(result["outputs"]) == all_names
        and all(
            r["above_absolute_tolerance"] == 0 and r["nonfinite_pairs"] == 0
            for r in result["outputs"].values()
        )
        and all(result["negative_controls"].values())
    )
    result["status"] = "author_csim_pass" if good else "author_or_tensor_check_failed"
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument("--summary", required=True, type=Path)
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--vitis-include", type=Path, default=Path("/vitis/2025.2/Vitis/include"))
    opt = ap.parse_args()
    opt.output = opt.output.resolve()
    opt.vitis_include = opt.vitis_include.resolve()
    # Generated host and kernel code declares large float arrays on the stack.
    soft, hard = resource.getrlimit(resource.RLIMIT_STACK)
    stack = 256 * 1024**2
    if soft != resource.RLIM_INFINITY and soft < stack:
        resource.setrlimit(
            resource.RLIMIT_STACK, (stack if hard == resource.RLIM_INFINITY else min(hard, stack), hard)
        )
    resource.setrlimit(resource.RLIMIT_CORE, (0, resource.getrlimit(resource.RLIMIT_CORE)[1]))
    cases = [
        c
        for c in json.loads((COLLECTION / "cases.json").read_text())
        if c["artifact_id"] == "stream_hls_official"
    ]
    index = {f["path"]: f for f in json.loads((COLLECTION / "files.json").read_text())["files"]}
    with ThreadPoolExecutor(max_workers=opt.jobs) as pool:
        futures = [pool.submit(run_case, case, index, opt) for case in cases]
        rows = [f.result() for f in futures]
    result = {
        "measurement": "author_host_cpp_float32",
        "compiler": subprocess.check_output(["g++", "--version"], text=True).splitlines()[0],
        "vitis_include": str(opt.vitis_include),
        "revision": next(
            a["commit"]
            for a in json.loads((COLLECTION / "artifacts.json").read_text())
            if a["id"] == "stream_hls_official"
        ),
        "absolute_tolerance": 0.1,
        "cases": rows,
    }
    opt.summary.parent.mkdir(parents=True, exist_ok=True)
    opt.summary.write_text(json.dumps(result, indent=2, allow_nan=False) + "\n")
    for row in rows:
        print(row["id"], row["status"], flush=True)


if __name__ == "__main__":
    main()
