"""Build a separate Light-HLS binary and recheck the three diagnosed inputs with Icarus."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import shlex
import subprocess

from paper_order_flow import BOARD_PATH, ROOT, compile_variant, hashfile, simulate


def build_compiler(output, recover_byte_gep):
    tests = ROOT / "Light_HLS/Tests"
    binary = output / "Light_HLS_Top"
    build = tests / "Light_HLS_Top/build"
    link = shlex.split((build / "CMakeFiles/Light_HLS_Top.dir/link.txt").read_text())
    link[link.index("-o") + 1] = str(binary)
    modules = ["HI_InstructionMoveBackward"]
    if recover_byte_gep:
        modules.append("HI_SeparateConstOffsetFromGEP")
    commands = []
    for module in modules:
        lib_build = tests / "LLVM_Learner_Libs/build/building" / module
        flags = lib_build / "CMakeFiles" / (module + ".dir") / "flags.make"
        options = []
        for line in flags.read_text().splitlines():
            field, _, value = line.partition("=")
            if field.strip() in {"CXX_DEFINES", "CXX_INCLUDES", "CXX_FLAGS"}:
                options.extend(shlex.split(value))
        archive = output / ("lib" + module + ".a")
        # The original archive may include more than this translation unit.
        commands.append(["cp", str(lib_build / archive.name), str(archive)])
        obj = output / (module + ".cc.o")
        source = ROOT / "Light_HLS/Implementations" / module / (module + ".cc")
        commands.append([link[0], *options, "-c", str(source), "-o", str(obj)])
        commands.append(["ar", "rcs", str(archive), str(obj)])
        link[link.index("-l" + module)] = str(archive)
    commands.append(link)
    (output / "build_commands.json").write_text(json.dumps(commands, indent=2) + "\n")
    with (output / "build.log").open("w") as log:
        for cmd in commands:
            subprocess.run(cmd, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
    return binary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--existing-run", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--recover-byte-gep", action="store_true")
    args = parser.parse_args()
    inputs = args.existing_run.resolve() / "input"
    out = args.output.resolve()
    out.mkdir(parents=True)
    binary = build_compiler(out, args.recover_byte_gep)

    def replay(name):
        source = inputs / ("vericert24_" + name)
        target = out / name
        stats = compile_variant(binary, BOARD_PATH, source / "kernel.cpp", target, None)
        result = simulate(source, target, stats)
        rtl = target / "hls/reports/rtl/top.v"
        native = subprocess.check_output([str(source / "oracle")], text=True).strip()
        result.update(oracle_hex=native, adapter_sha256=hashfile(source / "kernel.cpp"))
        if rtl.is_file():
            result["rtl_sha256"] = hashfile(rtl)
        (target / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        return name, result

    with ThreadPoolExecutor(max_workers=3) as pool:
        rows = dict(pool.map(replay, ["nussinov", "lu", "adi"]))
    result = {"compiler_sha256": hashfile(binary), "rows": rows}
    (out / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    for name, row in rows.items():
        print(name, row["status"], row.get("cycles"), row.get("checksum"))


if __name__ == "__main__":
    main()
