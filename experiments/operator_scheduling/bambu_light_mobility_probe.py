"""Test a static ASAP/ALAP priority on the frozen Light-HLS backend and inputs."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import shlex
import shutil
import subprocess

from flow_pilot import read_json, write_json
from paper_order_flow import BOARD_PATH, ROOT, compile_variant, hashfile, simulate, variant
from paper_sweep_summary import counts


# The native allocator's four weights are unchanged except for the experiment's
# explicit zero-vector selector. Static mobility = H - tail - earliest_start.
# H is common to the block, so a max-priority queue uses earliest_start + tail.
SELECTOR = [0, 0, 0, 0]
INSERTION = """    if (*weights == OrderWeights{})
    {
        std::vector<long long> earliest(ops.size(), 0);
        for (size_t i = 0; i < ops.size(); ++i)
        {
            const auto finish = earliest[i] + getLatency(context, *ops[i]);
            for (unsigned child : children[i])
                earliest[child] = std::max(earliest[child], finish);
        }
        ready.clear();
        for (size_t i = 0; i < ops.size(); ++i)
        {
            score[i] = earliest[i] + tail[i];
            if (!indegree[i])
                ready.emplace(-score[i], i);
        }
    }
"""


def build(directory):
    module = "HI_OperationScheduling"
    original = ROOT / "Light_HLS/Implementations" / module / (module + ".cc")
    source = directory / original.name
    anchor = "    std::vector<Instruction *> ordered;\n"
    content = original.read_text()
    if content.count(anchor) != 1:
        raise ValueError("allocator insertion point changed")
    source.write_text(content.replace(anchor, INSERTION + anchor))
    tests = ROOT / "Light_HLS/Tests"
    work = tests / "Light_HLS_Top/build"
    module_build = tests / "LLVM_Learner_Libs/build/building" / module
    flags = []
    for line in (module_build / f"CMakeFiles/{module}.dir/flags.make").read_text().splitlines():
        field, _, value = line.partition("=")
        if field.strip() in {"CXX_DEFINES", "CXX_INCLUDES", "CXX_FLAGS"}:
            flags.extend(shlex.split(value))
    link = shlex.split((work / "CMakeFiles/Light_HLS_Top.dir/link.txt").read_text())
    archive = directory / f"lib{module}.a"
    shutil.copy(module_build / archive.name, archive)
    obj = directory / f"{module}.cc.o"
    compiler = directory / "Light_HLS_Top"
    link[link.index("-o") + 1] = str(compiler)
    link[link.index(f"-l{module}")] = str(archive)
    commands = [[link[0], *flags, "-I", str(original.parent), "-c", str(source), "-o", str(obj)],
                ["ar", "rcs", str(archive), str(obj)], link]
    write_json(directory / "commands.json", commands)
    with (directory / "build.log").open("w") as log:
        for command in commands:
            subprocess.run(command, cwd=work, stdout=log, stderr=subprocess.STDOUT, check=True)
    return compiler, {"original_source": hashfile(original), "modified_source": hashfile(source),
                      "isolated_compiler": hashfile(compiler)}


def run_case(name, base, run, compiler):
    row = read_json(base / "native" / name / "experiment.json")
    inputs = base / "input" / name
    rtl = Path("hls/reports/rtl/top.v")
    original = base / "native" / name / rtl
    control = run / "unweighted" / name
    compiled = compile_variant(compiler, BOARD_PATH, inputs / "kernel.cpp", control, None)
    if compiled["status"] != "compiled" or hashfile(control / rtl) != hashfile(original):
        raise ValueError(f"unweighted RTL does not reproduce the frozen native for {name}")
    deployed = variant({"id": name}, "mobility", SELECTOR, run, compiler, BOARD_PATH)
    choice = deployed.get("choice")
    if deployed["status"] != "valid" or deployed["checksum"] != row["native"]["checksum"]:
        raise ValueError(f"deployed candidate failed for {name}")
    # Compare the direct ungated rankings too; keep the original experiment read-only.
    raw = {}
    for label, source in [("agent", base / "agent_order" / name), ("mobility", run / "mobility" / name)]:
        folder = run / (label + "_raw") / name
        folder.mkdir(parents=True)
        (folder / "hls").symlink_to(source / "hls", target_is_directory=True)
        result = simulate(inputs, folder, {"status":"compiled"})
        raw[label] = {"status":result["status"], "cycles":result.get("cycles"),
                      "checksum":result.get("checksum"), "rtl":hashfile(folder / rtl)}
    return name, {"split": row["split"], "native": row["native"]["cycles"],
                  "native_rtl":hashfile(original), "unweighted_rtl_identical":True,
                  "agent":row["agent"]["ordering"]["cycles"], "mobility":deployed["cycles"],
                  "selected_blocks":deployed["selected_blocks"], "choice":choice,
                  "checksum":deployed["checksum"], "raw":raw}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", required=True, type=Path)
    parser.add_argument("--run", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--jobs", default=5, type=int)
    args = parser.parse_args()
    run, base = args.run.resolve(), args.base.resolve()
    run.mkdir(parents=True)
    (run / "native").symlink_to(base / "native", target_is_directory=True)
    (run / "input").symlink_to(base / "input", target_is_directory=True)
    compiler, provenance = build(run)
    protocol = read_json(base / "protocol.json")
    names = protocol["dev"] + protocol["holdout"]
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        rows = dict(pool.map(lambda name: run_case(name, base, run, compiler), names))
    summary = {}
    for split in ("all", "dev", "holdout"):
        group = [r for r in rows.values() if split == "all" or split == r["split"]]
        summary[split] = {target: counts([(r["mobility"], r[target]) for r in group])
                          for target in ("native", "agent")}
        raw = [r for r in group if all(s["status"] == "valid" for s in r["raw"].values())]
        summary[split]["raw_mobility_vs_raw_agent"] = counts(
            [(r["raw"]["mobility"]["cycles"], r["raw"]["agent"]["cycles"]) for r in raw])
    write_json(args.output, {"provenance":provenance, "parent_protocol":hashfile(base / "protocol.json"),
                             "agent_weights":protocol["order"], "summary":summary, "cases":rows})
    print(summary, flush=True)


if __name__ == "__main__":
    main()
