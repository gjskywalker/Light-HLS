"""Probe collected source configurations on the native backend and on the policy replay entrypoint."""

import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import csv
import json
import os
from pathlib import Path
import re
import signal
import subprocess

from flow_pilot import COMPILER, ROOT, FREQUENCY
from run_round import digest, write_json

BUNDLE = ROOT / "benchmarks/hls_scheduling_2024_2026"
ESCAPE = re.compile(r"\x1b\[[0-9;]*m")


def command(args, cwd, env, label, timeout):
    argv = list(map(str, args))
    write_json(cwd / (label + ".argv.json"), argv)
    with (cwd / (label + ".log")).open("w") as log:
        process = subprocess.Popen(
            argv, cwd=cwd, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True
        )
        try:
            return process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
            return 124


def error_message(log, status):
    lines = [ESCAPE.sub("", line) for line in log.read_text(errors="replace").splitlines()]
    lines = [line.strip() for line in lines if line.strip()]
    for pattern in (
        "Light-HLS backend error",
        "mismatch",
        "AddressSanitizer:",
        "FATAL:",
        "fatal error:",
        "error:",
        "Failed to ",
        "Assertion ",
        "LLVM ERROR",
    ):
        for line in lines:
            if pattern in line:
                start = line.index(pattern)
                return line[start : start + 250]
    return "timeout" if status == 124 else (lines[-1][-220:] if lines else f"exit {status}")


def graph_diagnostic(work, status):
    manifest = work / "instrumented/reports/manifest.json"
    if manifest.is_file():
        errors = json.loads(manifest.read_text()).get("errors", [])
        if errors:
            return errors[0]["code"] + ": " + errors[0]["message"]
    return error_message(work / "export.log", status)


def paths_and_top(row, work):
    original = BUNDLE / row["source_path"]
    source, top = original, row.get("entrypoint") or original.stem
    include = [original.parent]
    changes = []
    artifact = row["artifact_id"]
    if artifact == "vericert_pldi2024_archive":
        source = work / original.name
        source.write_text("#define SYNTHESIS 1\n" + original.read_text())
        changes.append("archive Makefile defines SYNTHESIS")
    elif artifact == "dynamic_outlsq26":
        include.append(BUNDLE / "sources" / artifact / "include")
        changes.append("use matched Dynamatic Integration.h include path")
    elif artifact == "stream_hls_official":
        vitis = os.environ.get("XILINX_HLS")
        if vitis:
            include.append(Path(vitis) / "include")
            changes.append("enable Vitis vendor include path")
    elif artifact == "hybrid_hls_v1_zenodo3561115":
        helpers = [
            BUNDLE / name
            for name in row["bundle_files"]
            if Path(name).suffix == ".cpp" and Path(name).stem in ("g", "g1", "g2")
        ]
        if helpers:
            source = work / "unity.cpp"
            text = original.read_text()
            includes = {line.strip() for line in text.splitlines() if line.strip().startswith("#include")}
            for helper in helpers:
                text += (
                    "\n"
                    + "\n".join(
                        line for line in helper.read_text().splitlines() if line.strip() not in includes
                    )
                    + "\n"
                )
            source.write_text(text)
            changes.append("one translation unit with " + ", ".join(p.name for p in helpers))
    return source, top, include, changes


def main_checksum(src, rtl, work, env, timeout):
    """Check the original deterministic Vericert main() return value, including all 32 bits."""
    (work / "host.c").write_text(
        "#include <stdio.h>\nint bench_main(void);\n"
        'int main(void){printf("%08x\\n",(unsigned)bench_main());return 0;}\n'
    )
    host_obj = work / "host_kernel.o"
    rc = command(
        ["cc", "-O1", "-DSYNTHESIS", "-Dmain=bench_main", "-c", src, "-o", host_obj],
        work,
        env,
        "host_compile",
        timeout,
    )
    if rc or command(["cc", host_obj, "host.c", "-o", "host"], work, env, "host_link", timeout):
        return {"status": "host_build_failure"}
    if command([work / "host"], work, env, "host", timeout):
        return {"status": "host_run_failure"}
    expected = (work / "host.log").read_text().strip().splitlines()[-1]
    if not re.fullmatch(r"[0-9a-f]{8}", expected):
        return {"status": "missing_checksum"}
    tb = f"""module tb;
reg ap_clk=0, ap_rst=1, ap_start=0;
wire ap_done,ap_idle,ap_ready;
wire [31:0] ap_return;
main dut(.*);
always #5 ap_clk=~ap_clk;
integer cycles=0;
initial begin
 repeat(2) @(negedge ap_clk); ap_rst=0; ap_start=1;
 @(negedge ap_clk); ap_start=0;
 while (ap_done !== 1 && cycles < 10000000) begin @(negedge ap_clk); cycles=cycles+1; end
 if(ap_done!==1 || ap_return!==32'h{expected}) $fatal(1,"expected {expected} got %h cycles %0d",ap_return,cycles);
 $display("PASS %0d",cycles); $finish;
end endmodule"""
    (work / "tb.v").write_text(tb)
    if command(
        ["iverilog", "-g2012", "-s", "tb", "-o", "sim", rtl, "tb.v"], work, env, "sim_compile", timeout
    ):
        return {"status": "elaboration_failure", "expected": expected}
    rc = command(["vvp", "sim"], work, env, "sim", timeout)
    m = re.search(r"^PASS (\d+)$", (work / "sim.log").read_text(), re.M)
    if rc or not m:
        return {
            "status": "timeout" if rc == 124 else "mismatch_or_cycle_limit",
            "expected": expected,
            "detail": error_message(work / "sim.log", rc),
        }
    return {"status": "matched", "cycles": int(m[1]), "expected": expected}


def top_signature(source, top, env, work, timeout):
    frontend = "clang" if source.suffix == ".c" else "clang++"
    rc = command(
        [
            frontend,
            "-Xclang",
            "-ast-dump=json",
            "-Xclang",
            "-ast-dump-filter=" + top,
            "-fsyntax-only",
            source,
        ],
        work,
        env,
        "signature",
        timeout,
    )
    text = (work / "signature.log").read_text(errors="replace")
    if rc:
        raise ValueError("host signature parse failed")
    decoder = json.JSONDecoder()
    for match in re.finditer(r"^\{", text, re.M):
        node, _ = decoder.raw_decode(text, match.start())
        if (
            node.get("kind") == "FunctionDecl"
            and node.get("name") == top
            and any(child.get("kind") == "CompoundStmt" for child in node.get("inner", []))
        ):
            return (
                node["type"]["qualType"].split(" (")[0],
                [(p["name"], p["type"]["qualType"]) for p in node["inner"] if p["kind"] == "ParmVarDecl"],
            )
    raise ValueError("no matching definition")


def differential_cosim(row, work, top, source, env, timeout):
    native = work / "native/reports"
    rtl = native / "rtl/top.v"
    objects = {
        m["name"]: m
        for m in json.loads((native / "binding/binding.json").read_text())["memory_objects"]
        if m["storage"] == "external"
    }
    if any(
        m["protocol"] != "ap_memory" or m["banks"] != 1 or not m["depth_exact"] for m in objects.values()
    ):
        return {"status": "unsupported_external_memory_layout"}
    interface = rtl.read_text().partition(");")[0]
    pins = {
        name: (direction, width)
        for direction, width, name in re.findall(
            r"^\s*(input|output)\s+(?:wire|reg)\s+(\[\d+:0\]\s*)?(\w+)", interface, re.M
        )
    }
    if not pins:
        return {"status": "unsupported_top_interface"}
    try:
        ret, args = top_signature(source, top, env, work, timeout)
    except (ValueError, KeyError) as error:
        return {"status": "oracle_setup_failed", "detail": str(error)}
    host = [
        "#include <stdio.h>",
        "#include <stdint.h>",
        "#include <string.h>",
        "#define main paper_embedded_main",
        f'#include "{source}"',
        "#undef main",
        'static void dump(FILE *f,const void *p,size_t n){ uint64_t w=0; memcpy(&w,p,n); fprintf(f,"%llx\\n",(unsigned long long)w); }',
        "int main(void){",
    ]
    decl = []
    sv = ["module paper_tb;"]
    for name, (direction, width) in pins.items():
        sv.append(f'{"reg" if direction=="input" else "wire"} {width} {name};')
    sv.extend(
        [
            f"{top} dut(.*);",
            "always #5 ap_clk=~ap_clk;",
            "integer i, cycles, row;",
            "reg [4095:0] filename;",
        ]
    )
    before, after, mem_written = [], [], []
    used = {"ap_clk", "ap_rst", "ap_start", "ap_idle", "ap_ready", "ap_done", "ap_return"}
    for j, (name, typ) in enumerate(args):
        local = f"sweep_arg_{name}"
        m = objects.get(name)
        if not m and "*" in typ:
            return {"status": "pointer_without_external_depth", "detail": name}
        if m:
            depth, width, port = m["depth"], m["element_width"], m["port_name"]
            if width not in (8, 16, 32, 64):
                return {"status": "unsupported_element_width", "detail": width}
            nested = re.fullmatch(r"(.+?)\s*\(\*\)\s*((?:\[\d+\])+)", typ)
            if nested:
                element, tail = nested.groups()
                elements_per_row = 1
                for n in re.findall(r"\d+", tail):
                    elements_per_row *= int(n)
                if depth % elements_per_row:
                    return {"status": "inconsistent_array_dimensions", "detail": name}
                decl.append(f"{element} {local}[{depth//elements_per_row}]{tail};")
            elif typ.endswith("*"):
                element = typ[:-1].strip()
                decl.append(f"{element} {local}[{depth}];")
            else:
                return {"status": "unsupported_array_type", "detail": typ}
            index_arg = any(k in name.lower() for k in ("addr", "idx", "index", "col", "feature"))
            value = "(i*13+row)%7" if index_arg else "(i*3+(i/7)+row*11+" + str(j) + ")%4"
            if not index_arg and top.lower() in ("gsum", "gsumif", "if_loop_add"):
                # These reductions predicate on the sign of the input data.
                value = f"((int)({value}) - (row == 0 ? 0 : row))"
            if top == "matrix_power" and name in ("row", "col", "a"):
                # A destination permutation bounds signed accumulation through repeated iterations.
                value = {"row": f"(i+row)%{depth}", "col": f"(i+{depth}-row)%{depth}", "a": "(i+row)%2"}[
                    name
                ]
            if top == "vecNormTrans" and name == "a":
                # The original driver mixes fractional and large inputs to keep the denominator nonzero.
                value = "(i%2 == 0 ? 2.0f : (row+1)*0.1f)"
            before.append(
                f"for(size_t i=0;i<{depth};i++){{ {element} sweep_input_word=({element})({value}); "
                f"memcpy((char*){local}+i*sizeof(sweep_input_word),&sweep_input_word,sizeof(sweep_input_word)); }}"
            )
            sv.extend(
                [
                    f"reg [{width-1}:0] mem_{name}[0:{depth-1}];",
                    f"reg [{width-1}:0] exp_{name}[0:{depth-1}];",
                ]
            )
            for usage in m["port_usage"]:
                k = usage["port"]
                base = port + "_"
                ce = base + "ce" + str(k)
                addr = base + "address" + str(k)
                used.update([ce, addr])
                if addr not in pins or ce not in pins:
                    return {"status": "unmapped_memory_port", "detail": port}
                sv.append(f"always @(posedge ap_clk) if ({ce}) begin")
                sv.append(
                    f'if (^{addr} === 1\'bx || {addr} >= {depth}) $fatal(1,"invalid address {name} %d",{addr});'
                )
                if usage["read"]:
                    q = base + "q" + str(k)
                    used.add(q)
                    sv.append(f"{q} <= mem_{name}[{addr}];")
                if usage["write"]:
                    we = base + "we" + str(k)
                    data = base + "d" + str(k)
                    used.update([we, data])
                    sv.append(f"if ({we}) mem_{name}[{addr}] <= {data};")
                sv.append("end")
            for role, dest in [("before", before), ("after", after)]:
                dest.append(
                    f'snprintf(file,sizeof(file),"v%d_{role}_{name}.hex",row); f=fopen(file,"w"); '
                    f"for(size_t i=0;i<{depth};i++) dump(f,(char*){local}+i*sizeof({element}),sizeof({element})); fclose(f);"
                )
            mem_written.append((name, depth))
        else:
            decl.append(f"{typ} {local};")
            value = (
                "(row%2)"
                if top == "iir" and name == "a"
                else "(3+row)"
                if name.lower() in ("n", "size", "count", "len")
                else "(1+(row%2))"
            )
            before.append(f"{local}=({typ}){value};")
            if name in pins:
                used.add(name)
                before.append(
                    f'snprintf(file,sizeof(file),"v%d_scalar_{name}.hex",row); f=fopen(file,"w"); '
                    f"dump(f,&{local},sizeof({local})); fclose(f);"
                )
                sv.append(f"reg {pins[name][1]} vec_{name}[0:0];")
    missing = set(pins) - used
    if missing:
        return {"status": "unmapped_rtl_ports", "detail": sorted(missing)}
    call = f"{top}(" + ",".join("sweep_arg_" + k for k, _ in args) + ")"
    host.extend(decl + ["for(int row=0;row<4;row++){ char file[256]; FILE *f;"] + before)
    if ret != "void":
        if "ap_return" not in pins:
            return {"status": "return_port_missing"}
        host.extend(
            [
                f"{ret} result={call};",
                'snprintf(file,sizeof(file),"v%d_return.hex",row); f=fopen(file,"w"); dump(f,&result,sizeof(result)); fclose(f);',
            ]
        )
        sv.append(f'reg {pins["ap_return"][1]} exp_return[0:0];')
    else:
        host.append(call + ";")
    host.extend(after + ["} return 0; }"])
    ext = ".c" if source.suffix == ".c" else ".cpp"
    wrapper = work / ("vector_oracle" + ext)
    wrapper.write_text("\n".join(host) + "\n")
    rc = command(
        [
            "clang" if ext == ".c" else "clang++",
            "-O1",
            "-ffp-contract=off",
            "-fsanitize=address,undefined",
            "-fno-sanitize-recover=all",
            wrapper,
            "-o",
            "oracle",
        ],
        work,
        env,
        "oracle_compile",
        timeout,
    )
    if rc:
        return {"status": "oracle_build_failed", "detail": error_message(work / "oracle_compile.log", rc)}
    oracle_env = dict(env, ASAN_OPTIONS="detect_leaks=0")
    rc = command([work / "oracle"], work, oracle_env, "oracle_vectors", timeout)
    if rc:
        return {
            "status": "oracle_rejected_inputs",
            "detail": error_message(work / "oracle_vectors.log", rc),
        }
    sv.append("initial begin ap_clk=0; ap_rst=1; ap_start=0; for(row=0;row<4;row=row+1) begin")
    sv.append("@(negedge ap_clk); ap_rst=1;")
    for name, depth in mem_written:
        for role, mem in [("before", "mem"), ("after", "exp")]:
            sv.append(
                f'$sformat(filename,"v%0d_{role}_{name}.hex",row); $readmemh(filename,{mem}_{name});'
            )
    for name, typ in args:
        if name in pins and name not in objects:
            sv.append(
                f'$sformat(filename,"v%0d_scalar_{name}.hex",row); $readmemh(filename,vec_{name}); {name}=vec_{name}[0];'
            )
    if ret != "void":
        sv.append('$sformat(filename,"v%0d_return.hex",row); $readmemh(filename,exp_return);')
    sv.extend(
        [
            "@(negedge ap_clk); ap_rst=0; ap_start=1; @(negedge ap_clk); ap_start=0; cycles=0;",
            "while(ap_done !== 1 && cycles<10000000) begin @(negedge ap_clk); cycles=cycles+1; end",
            'if(ap_done!==1) $fatal(1,"cycle_limit");',
        ]
    )
    if ret != "void":
        sv.append(
            'if(ap_return!==exp_return[0]) $fatal(1,"return mismatch %h %h",ap_return,exp_return[0]);'
        )
    for name, depth in mem_written:
        sv.append(
            f'for(i=0;i<{depth};i=i+1) if(mem_{name}[i]!==exp_{name}[i]) $fatal(1,"{name}[%d] mismatch %h %h",i,mem_{name}[i],exp_{name}[i]);'
        )
    sv.extend(['$display("MATCH %0d %0d",row,cycles); end $finish; end endmodule'])
    tb = work / "paper_tb.v"
    tb.write_text("\n".join(sv) + "\n")
    rc = command(
        ["iverilog", "-g2012", "-s", "paper_tb", "-o", "paper_sim", rtl, tb],
        work,
        env,
        "paper_elaboration",
        timeout,
    )
    if rc:
        return {
            "status": "elaboration_failure",
            "detail": error_message(work / "paper_elaboration.log", rc),
        }
    rc = command(["vvp", "paper_sim"], work, env, "paper_sim", timeout)
    values = [
        int(v) for v in re.findall(r"^MATCH \d+ (\d+)$", (work / "paper_sim.log").read_text(), re.M)
    ]
    if rc or len(values) != 4:
        return {
            "status": "timeout" if rc == 124 else "rtl_mismatch_or_cycle_limit",
            "passed_vectors": len(values),
            "detail": error_message(work / "paper_sim.log", rc),
        }
    return {"status": "matched", "cycles": values, "vectors": 4}


def probe(row, args):
    result = {key: row[key] for key in ("id", "paper_ids", "artifact_id", "mapping_confidence", "sha256")}
    original = BUNDLE / row["source_path"]
    if digest(original) != row["sha256"]:
        raise ValueError(f"Source hash mismatch: {row['id']}")
    work = args.output / row["id"]
    work.mkdir()
    result["functional"] = {"status": "not_run"}
    if original.suffix == ".x" or row["artifact_id"] == "hybrid_dlf_zenodo14383450":
        result["status"] = (
            "requires_xls_dslx" if original.suffix == ".x" else "requires_intel_sycl_device_flow"
        )
        return result
    source, top, include, changes = paths_and_top(row, work)
    result.update(top=top, source_setup=changes)
    env = dict(
        os.environ,
        HI_HLS_LIB_PATH=str(ROOT / "Light_HLS/HLS_Data_Lib"),
        CPATH=os.pathsep.join(map(str, include)),
    )
    for key in (
        "HI_EXPERIMENT_SCHEDULE_GRAPH",
        "HI_EXPERIMENT_SCHEDULE_REPLAY",
        "HI_EXPERIMENT_ORDER_WEIGHTS",
        "HI_EXPERIMENT_ORDER_BLOCKS",
    ):
        env.pop(key, None)
    common = [
        args.compiler,
        source,
        top,
        "--board",
        "u250",
        "--freq",
        FREQUENCY,
        "--synthesis",
        "disable",
        "--output",
    ]
    native = work / "native"
    rc = command([*common, native], work, env, "native", args.timeout)
    rtl = native / "reports/rtl/top.v"
    if rc or not rtl.is_file():
        result.update(
            status="native_compile_failed", native_exit=rc, detail=error_message(work / "native.log", rc)
        )
        return result
    result["status"] = "rtl_emitted"
    result["rtl_sha256"] = digest(rtl)
    env["HI_EXPERIMENT_SCHEDULE_GRAPH"] = str(work / "graph.json")
    rc = command([*common, work / "instrumented"], work, env, "export", args.timeout)
    if rc or not (work / "graph.json").is_file():
        result["graph_export"] = graph_diagnostic(work, rc)
        result["graph_export_exit"] = rc
    else:
        graph = json.loads((work / "graph.json").read_text())
        result.update(
            graph_export="exported", nodes=len(graph["nodes"]), graph_cycles=graph["native_cycles"]
        )
    if row["artifact_id"] == "vericert_pldi2024_archive":
        result["functional"] = main_checksum(original, rtl, work, env, args.sim_timeout)
    else:
        rc = command(
            ["iverilog", "-g2012", "-s", top, "-o", work / "elaborated", rtl],
            work,
            env,
            "elaboration",
            args.timeout,
        )
        result["elaboration"] = "passed" if not rc else error_message(work / "elaboration.log", rc)
        if not rc:
            result["functional"] = differential_cosim(row, work, top, source, env, args.sim_timeout)
    return result


def compare_policy(row, work, native_work, compiler, weights, timeout, sim_timeout, blocks=None):
    """Recompile with a frozen policy and run the same native-oracle vectors and testbench."""
    work.mkdir()
    source, top, include, _ = paths_and_top(row, work)
    env = dict(
        os.environ,
        CPATH=os.pathsep.join(map(str, include)),
        HI_HLS_LIB_PATH=str(ROOT / "Light_HLS/HLS_Data_Lib"),
        HI_EXPERIMENT_ORDER_WEIGHTS=json.dumps(weights),
    )
    for key in (
        "HI_EXPERIMENT_SCHEDULE_GRAPH",
        "HI_EXPERIMENT_SCHEDULE_REPLAY",
        "HI_EXPERIMENT_ORDER_BLOCKS",
    ):
        env.pop(key, None)
    if blocks is not None:
        env["HI_EXPERIMENT_ORDER_BLOCKS"] = json.dumps(blocks)
    rc = command(
        [
            compiler,
            source,
            top,
            "--board",
            "u250",
            "--freq",
            FREQUENCY,
            "--synthesis",
            "disable",
            "--output",
            work / "candidate",
        ],
        work,
        env,
        "compile",
        timeout,
    )
    rtl = work / "candidate/reports/rtl/top.v"
    result = {"id": row["id"], "family_group": row["family_group"], "sha256": row["sha256"]}
    if rc or not rtl.exists():
        return dict(result, status="compile_failed", detail=error_message(work / "compile.log", rc))
    for data in native_work.glob("v*.hex"):
        (work / data.name).symlink_to(data)
    tb = native_work / "paper_tb.v"
    rc = command(
        ["iverilog", "-g2012", "-s", "paper_tb", "-o", "sim", rtl, tb], work, env, "elab", timeout
    )
    if rc:
        return dict(result, status="elaboration_failed", detail=error_message(work / "elab.log", rc))
    rc = command(["vvp", "sim"], work, env, "sim", sim_timeout)
    text = (work / "sim.log").read_text()
    cycles = [int(v) for v in re.findall(r"^MATCH \d+ (\d+)$", text, re.M)]
    if rc or len(cycles) != 4:
        return dict(result, status="rtl_failure", detail=error_message(work / "sim.log", rc))
    return dict(result, status="matched", cycles=cycles)


def gate_policy(row, work, native_work, compiler, weights, raw, native_cycles, timeout, sim_timeout):
    """Apply the same compile-time gate as the Vericert development workflow."""
    from paper_order_flow import choose_blocks

    def costs(path):
        schedule = path / "reports/scheduling/schedule.json"
        if not schedule.is_file():
            return {"status": "failed", "states": {}}
        report = json.loads(schedule.read_text())
        return {
            "status": "compiled" if report["valid"] else "failed",
            "states": {b["name"]: b["state_count"] for b in report["blocks"]},
        }

    native = costs(native_work / "native")
    candidate = costs(work / "candidate")
    improved = choose_blocks(native, candidate)
    if not improved:
        return {"selection": "native", "cycles": native_cycles, "selected_blocks": []}
    if not any(candidate["states"][b] > n for b, n in native["states"].items()):
        return {
            "selection": "policy",
            "status": raw["status"],
            "cycles": raw.get("cycles"),
            "selected_blocks": improved,
        }
    mix = compare_policy(
        row, work / "mixed", native_work, compiler, weights, timeout, sim_timeout, blocks=improved
    )
    mixed_costs = costs(work / "mixed/candidate")
    if (
        mixed_costs["status"] != "compiled"
        or mixed_costs["states"].keys() != native["states"].keys()
        or any(v > native["states"][b] for b, v in mixed_costs["states"].items())
    ):
        return {
            "selection": "native_after_cost_check",
            "cycles": native_cycles,
            "selected_blocks": improved,
        }
    return {
        "selection": "mixed",
        "status": mix["status"],
        "cycles": mix.get("cycles"),
        "selected_blocks": improved,
    }


def archive_driver(row, work, native, timeout):
    """Use the archive's CALL_KERNEL tracer to produce real input/output bytes."""
    work.mkdir()
    source, top, includes, _ = paths_and_top(row, work)
    env = dict(os.environ, CPATH=os.pathsep.join(map(str, includes)), ASAN_OPTIONS="detect_leaks=0")
    for name in ("INPUT_VECTORS", "C_OUT"):
        (work / name).mkdir()
    rc = command(
        [
            "clang++",
            "-x",
            "c++",
            "-std=c++17",
            "-O1",
            "-ffp-contract=off",
            "-fsanitize=address,undefined",
            "-fno-sanitize-recover=all",
            "-DHLS_VERIFICATION",
            source,
            "-o",
            "host",
        ],
        work,
        env,
        "host_compile",
        timeout,
    )
    if rc:
        return {
            "id": row["id"],
            "status": "author_driver_compile_failed",
            "detail": error_message(work / "host_compile.log", rc),
        }
    rc = command([work / "host"], work, env, "host_vectors", timeout)
    if rc:
        return {
            "id": row["id"],
            "status": "author_driver_rejected",
            "detail": error_message(work / "host_vectors.log", rc),
        }
    _, args = top_signature(source, top, env, work, timeout)
    call = re.search(r"CALL_KERNEL\s*\(\s*" + top + r"\s*,([^;]+)\);", source.read_text())
    actual = [s.strip() for s in call[1].split(",")]
    mapping = {name: arg for (name, _), arg in zip(args, actual, strict=True)}
    tb = (native / "paper_tb.v").read_text().replace("row<4", "row<1")
    for role, name in re.findall(r"v%0d_(before|after|scalar)_(\w+)\.hex", tb) + [("return", "out0")]:
        if role == "return" and "v%0d_return.hex" not in tb:
            continue
        inp = role in ("before", "scalar")
        prefix = "INPUT_VECTORS/input_" if inp else "C_OUT/output_"
        f = work / (prefix + mapping.get(name, name) + ".dat")
        if not f.exists():
            return {"id": row["id"], "status": "missing_traced_value", "detail": f.name}
        src = f.read_text()
        vals = re.findall(r"^0x([0-9a-fA-F]+)$", src, re.M)
        if src.count("[[transaction]]") != 1 or not vals:
            raise ValueError(f"unsupported archive trace in {f}")
        out = f"v0_{role}_{name}.hex" if role != "return" else "v0_return.hex"
        (work / out).write_text("\n".join(vals) + "\n")
    (work / "paper_tb.v").write_text(tb)
    return {
        "id": row["id"],
        **replay_archived_input(native / "native/reports/rtl/top.v", work, "native", timeout),
    }


def replay_archived_input(rtl, work, label, timeout):
    image = label + "_sim"
    env = os.environ.copy()
    rc = command(
        ["iverilog", "-g2012", "-s", "paper_tb", "-o", image, rtl, "paper_tb.v"],
        work,
        env,
        label + "_elab",
        timeout,
    )
    if rc:
        return {"status": "elaboration_failed", "detail": error_message(work / (label + "_elab.log"), rc)}
    rc = command(["vvp", image], work, env, label, timeout)
    log = work / (label + ".log")
    verified = re.search(r"^MATCH 0 (\d+)$", log.read_text(), re.M)
    if rc or not verified:
        return {"status": "rtl_failure", "detail": error_message(log, rc)}
    return {"status": "matched", "rtl_cycles": int(verified[1])}


def archive_comparison(row, args):
    work = args.output / row["id"]
    record = archive_driver(row, work, args.archived_outlsq.resolve() / row["id"], args.sim_timeout)
    if args.author_policies and record["status"] == "matched":
        for name, directory in zip(("critical", "agent"), args.author_policies, strict=True):
            rtl = directory.resolve() / row["id"] / "candidate/reports/rtl/top.v"
            replay = replay_archived_input(rtl, work, name, args.sim_timeout)
            if replay["status"] == "matched":
                record[name + "_cycles"] = replay["rtl_cycles"]
            else:
                record[name + "_failure"] = replay
    if args.author_artifact:
        design = row["id"].rsplit("__", 1)[-1]
        for name in ("Ours", "Baseline"):
            files = list(
                (args.author_artifact / "skip/results" / name / "Best").glob(design + "-*/results.csv")
            )
            if len(files) != 1:
                raise ValueError(f"Expected a unique {name} result for {design}, got {files}")
            with files[0].open() as source:
                (published,) = csv.DictReader(source)
            if published["Benchmark"] != design:
                raise ValueError(f"Unexpected benchmark in {files[0]}")
            record[name + "_paper_cycles"] = int(published["Cycles"])
            record[name + "_paper_real_period_ns"] = float(published["Real CP"])
            record[name + "_paper_row"] = str(files[0].relative_to(args.author_artifact))
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--summary", type=Path, required=True)
    parser.add_argument("--compiler", type=Path, default=COMPILER)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--timeout", type=int, default=75)
    parser.add_argument("--sim-timeout", type=int, default=90)
    parser.add_argument(
        "--baseline-run", type=Path, help="Frozen baseline run with the native C-oracle vectors"
    )
    parser.add_argument("--baseline-summary", type=Path)
    parser.add_argument("--weights", type=json.loads, help="JSON array of four integer priorities")
    parser.add_argument(
        "--archived-outlsq",
        type=Path,
        help="Directory containing validated testbenches for the archived driver audit",
    )
    parser.add_argument(
        "--author-policies",
        type=Path,
        nargs=2,
        metavar=("CRITICAL_RUN", "AGENT_RUN"),
        help="Replay archived input on the two already-compiled policy RTL directories",
    )
    parser.add_argument(
        "--author-artifact",
        type=Path,
        help="Path to the published Out with LSQs artifact with skip/results directories",
    )
    args = parser.parse_args()
    if (args.author_policies or args.author_artifact) and not args.archived_outlsq:
        parser.error("--author-policies and --author-artifact require --archived-outlsq")
    if any((args.baseline_run, args.baseline_summary, args.weights)) and not all(
        (args.baseline_run, args.baseline_summary, args.weights)
    ):
        parser.error("comparison requires --baseline-run, --baseline-summary, and --weights")
    args.compiler = args.compiler.resolve()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    rows = [
        r for r in json.loads((BUNDLE / "cases.json").read_text()) if r.get("track") != "graph_auxiliary"
    ]
    if args.archived_outlsq:
        rows = [r for r in rows if r["artifact_id"] == "dynamic_outlsq26"]
        with ThreadPoolExecutor(args.jobs) as pool:
            records = list(pool.map(lambda row: archive_comparison(row, args), rows))
    elif args.baseline_run:
        history = json.loads(args.baseline_summary.read_text())
        if digest(args.compiler) != history["compiler_sha256"]:
            parser.error("comparison must use the same compiler binary as baseline")
        native = {
            r["id"]: r["functional"]["cycles"]
            for r in history["cases"]
            if r["artifact_id"] != "vericert_pldi2024_archive" and r["functional"]["status"] == "matched"
        }
        rows = [r for r in rows if r["id"] in native]
        with ThreadPoolExecutor(args.jobs) as pool:
            records = list(
                pool.map(
                    lambda r: compare_policy(
                        r,
                        args.output / r["id"],
                        args.baseline_run.resolve() / r["id"],
                        args.compiler,
                        args.weights,
                        args.timeout,
                        args.sim_timeout,
                    ),
                    rows,
                )
            )
        for row, r in zip(rows, records, strict=True):
            r["native_cycles"] = native[r["id"]]
            r["gate"] = gate_policy(
                row,
                args.output / r["id"],
                args.baseline_run.resolve() / r["id"],
                args.compiler,
                args.weights,
                r,
                r["native_cycles"],
                args.timeout,
                args.sim_timeout,
            )
    else:
        with ThreadPoolExecutor(args.jobs) as pool:
            records = list(pool.map(lambda r: probe(r, args), rows))
    out = {
        "board": "u250",
        "frequency": FREQUENCY,
        "synthesis": False,
        "compiler_sha256": digest(args.compiler),
        "manifest_sha256": digest(BUNDLE / "cases.json"),
        "timeout_seconds": args.timeout,
        "sim_timeout_seconds": args.sim_timeout,
        "jobs": args.jobs,
        "weights": args.weights,
        "cases": records,
    }
    write_json(args.summary, out)
    print(Counter(r["status"] for r in records), flush=True)


if __name__ == "__main__":
    main()
