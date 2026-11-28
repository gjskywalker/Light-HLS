"""Compare every observable output array of the final Vericert policies with its native C array."""

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess
import sys

from paper_physical_final_route import final_rtl, hash_file, read


def output_arrays(source):
    entry = source.index("int benchmark_main()")
    call = re.search(r"return\s+(?:print|check)_array\s*\(([^;]*)\)\s*;", source[entry:])
    declarations = dict(re.findall(r"int\s+\(?(\w+)\)?\s*((?:\[[^\]]+\]\s*)+)\s*;", source[entry:]))
    arrays = {}
    for arg in call[1].split(","):
        name = arg.strip()
        if name not in declarations:
            continue
        shape = []
        for size in re.findall(r"\[([^\]]+)\]", declarations[name]):
            if not re.fullmatch(r"\d+(?:\s*\+\s*\d+)*", size):
                raise ValueError(f"Nonconstant array dimension: {name} {size}")
            shape.append(sum(map(int, size.split("+"))))
        arrays[name] = shape
    if not arrays:
        raise ValueError("No output arrays identified in final C check")
    return arrays, entry + call.start()


def reference(source, folder, arrays, position, expected):
    # fwrite reads a whole C object, including all dimensions in native row-major order.
    observation = []
    for name in arrays:
        observation.append(f'{{ FILE *f = fopen("{name}.bin", "wb"); '
                           f'if (!f || fwrite(&{name}, sizeof({name}), 1, f) != 1) abort(); fclose(f); }}')
    code = '#include <stdio.h>\n#include <stdlib.h>\n_Static_assert(sizeof(int)==4,"32-bit int required");\n'
    code += source[:position] + "\n".join(observation) + "\n" + source[position:]
    (folder / "observed.c").write_text(code)
    results = []
    for flag in ("-O1", "-O0"):
        subprocess.run(["clang", "-std=c11", flag, "observed.c", "-lm", "-o", "oracle"], cwd=folder, check=True)
        checksum = subprocess.check_output(["./oracle"], cwd=folder, text=True).strip()
        if checksum != expected:
            raise ValueError(f"C observer returned {checksum}, previous native checksum was {expected}")
        values = {}
        for name, shape in arrays.items():
            raw = (folder / (name + ".bin")).read_bytes()
            if len(raw) != 4 * math.prod(shape):
                raise ValueError(f"Unexpected native size for {name}")
            values[name] = [f"{int.from_bytes(raw[i:i+4], sys.byteorder):08x}" for i in range(0,len(raw),4)]
        results.append(values)
    if results[0] != results[1]:
        raise ValueError("Observed C arrays differ between -O1 and -O0")
    return results[0]


def digest(words):
    return hashlib.sha256(("".join(word + "\n" for word in words)).encode()).hexdigest()


def simulate(folder, rtl, arrays, expected, expected_cycles):
    folder.mkdir()
    rtl_text = rtl.read_text()
    memory = dict((name, int(last) + 1) for name, last in
                  re.findall(r"reg \[31:0\] (\w+)_storage \[0:(\d+)\];", rtl_text))
    probes = []
    for name, dims in arrays.items():
        symbol = "table_v" if name == "table" else name
        size = math.prod(dims)
        if memory.get(symbol) != size:
            raise ValueError(f"Missing 32-bit RAM corresponding to {name}, expected depth {size}")
        probes.append(f'file = $fopen("{name}.hex", "w");\n'
                      f'    for (i=0; i<{size}; i=i+1) $fdisplay(file,"%08h",dut.{symbol}_storage[i]);\n'
                      '    $fclose(file);')
    (folder / "tb.sv").write_text(f"""module tb;
  reg ap_clk=0, ap_rst=1, ap_start=0;
  wire ap_done, ap_idle, ap_ready; wire [31:0] ap_return;
  integer i, file, cycles=0;
  benchmark_main dut(.*);
  always #5 ap_clk=~ap_clk;
  initial begin
    repeat (2) @(negedge ap_clk); ap_rst=0; ap_start=1;
    @(negedge ap_clk); ap_start=0;
    while (ap_done !== 1'b1 && cycles <= {expected_cycles}) begin
      @(negedge ap_clk); cycles=cycles+1;
    end
    if (ap_done !== 1'b1 || cycles != {expected_cycles} || ap_return !== 32'h{expected})
      $fatal(1,"done=%b cycles=%0d return=%h",ap_done,cycles,ap_return);
    $display("cycles=%0d checksum=%08h",cycles,ap_return);
    {chr(10).join(probes)}
    $finish;
  end
endmodule
""")
    subprocess.run(["iverilog", "-g2012", "-s", "tb", "-o", "sim", str(rtl), "tb.sv"],
                   cwd=folder, check=True)
    trace = subprocess.check_output(["vvp", "sim"], cwd=folder, text=True, timeout=360)
    (folder / "simulation.log").write_text(trace)
    return {name: (folder / (name + ".hex")).read_text().splitlines() for name in arrays}


def check(case, baseline, record, single, joint, out):
    folder = out / case
    folder.mkdir()
    oracle = single / "input" / case / "oracle.c"
    source = oracle.read_text()
    expected = baseline["native"]["checksum"]
    arrays, position = output_arrays(source)
    refs = reference(source, folder, arrays, position, expected)
    observed_rtl = {}
    case_row = {"oracle_source_sha256": hash_file(oracle), "checksum": expected}
    for role in ("agent", "manual"):
        rtl, selection = final_rtl(single, joint, case, role, record)
        cycles = record[role]["cycles"]
        sha = hash_file(rtl)
        if sha not in observed_rtl:
            observed_rtl[sha] = (cycles, simulate(folder / role, rtl, arrays, expected, cycles), role)
        verified_cycles, values, simulated_as = observed_rtl[sha]
        if cycles != verified_cycles:
            raise ValueError(f"{case} same RTL, different expected cycle counts")
        output = {}
        for name, dims in arrays.items():
            words, ref = values[name], refs[name]
            if len(words) != len(ref):
                raise ValueError(f"{case} {name}: incomplete memory dump")
            diffs = [i for i, (a,b) in enumerate(zip(words, ref)) if a != b]
            output[name] = {"dims": dims, "words": len(ref), "native_distinct": len(set(ref)),
                            "native_nonzero": sum(w != "00000000" for w in ref),
                            "unknown_words": sum(any(c in w for c in "xz") for w in words),
                            "mismatches": len(diffs), "examples_flat_rtl_c": [[i, words[i], ref[i]] for i in diffs[:5]],
                            "native_sha256": digest(ref), "rtl_sha256": digest(words)}
        case_row[role] = {"cycles": cycles, "choice": selection, "source": record[role]["source"],
                          "rtl_path": str(rtl.relative_to(single if record[role]["source"]=="single" else joint)),
                          "rtl_sha256": sha, "simulated_as": simulated_as, "arrays": output}
    (folder / "result.json").write_text(json.dumps(case_row, indent=2) + "\n")
    return case, case_row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("single", "joint", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    single, joint, out = (d.resolve() for d in (args.single, args.joint, args.output))
    base = read(single / "results.json")
    deployed = read(joint / "results.json")
    if deployed["parent_run"] != hash_file(single / "results.json"):
        raise ValueError("Frozen result lineage differs")
    out.mkdir(parents=True)
    def priority(case):
        paper = base["cases"][case]["published"]
        fastest = min(int(v) for k,v in paper.items() if k != "design")
        return (deployed["cases"][case]["agent"]["cycles"] >= fastest and
                case.split("_",1)[1] not in {"adi", "lu", "nussinov"}, case)
    rows = {}
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        work = [pool.submit(check, case, base["cases"][case], deployed["cases"][case], single, joint, out)
                for case in sorted(deployed["cases"], key=priority)]
        for job in as_completed(work):
            case, rows_case = job.result()
            rows[case] = rows_case
            mismatches = {role:sum(a["mismatches"] for a in rows_case[role]["arrays"].values())
                          for role in ("agent", "manual")}
            print(case, mismatches, flush=True)
    result = {"single_results_sha256": hash_file(single / "results.json"),
              "joint_results_sha256": hash_file(joint / "results.json"),
              "compiler_sha256": read(single / "protocol.json")["compiler_sha256"],
              "cases": dict(sorted(rows.items()))}
    (out / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    if any(a["mismatches"] for r in rows.values() for g in ("agent","manual") for a in r[g]["arrays"].values()):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
