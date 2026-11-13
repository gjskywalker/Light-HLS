#!/usr/bin/env python3
"""Bounded source/native-IR/RTL verification for the unmodified CV0 corpus.

Integer and IEEE binary16/binary32/binary64 scalar/array bit patterns are modeled. Unsupported types,
source UB, invalid fixtures and RTL failures remain distinct recorded outcomes.
"""

from __future__ import annotations

import argparse
import ast
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import random
import re
import shutil
import struct
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[2]
TYPES = {"int": ("i", 32), "unsigned int": ("I", 32), "char": ("b", 8),
         "short": ("h", 16), "short int": ("h", 16),
         "unsigned short": ("H", 16), "unsigned short int": ("H", 16),
         "long": ("q", 64), "long int": ("q", 64),
         "unsigned long": ("Q", 64), "unsigned long int": ("Q", 64),
         "long long": ("q", 64), "long long int": ("q", 64),
         "unsigned long long": ("Q", 64), "unsigned long long int": ("Q", 64),
         "signed char": ("b", 8), "unsigned char": ("B", 8), "bool": ("?", 1),
         "_Float16": ("e", 16), "float": ("f", 32), "double": ("d", 64)}


def type_info(argument):
    if argument.get("base") == "bool" and argument.get("depth"):
        return "?", 8
    return TYPES[argument.get("base", "int")]


def encode_value(value, base):
    return struct.pack("<" + TYPES[base][0], value)


def bit_value(value, base):
    return int.from_bytes(encode_value(value, base), "little")


def equal_word(left, right, base):
    if left == right:
        return True
    if base not in ("_Float16", "float", "double"):
        return False
    fmt = TYPES[base][0]
    return math.isnan(struct.unpack("<" + fmt, left)[0]) and math.isnan(struct.unpack("<" + fmt, right)[0])
POINTER_DEPTHS = {"random28": {"dlti": 128, "bli": 128},
                  "random12": {"s": 160, "L_ACF": 9},
                  "random38": {"k": 32, "rc": 1}, "random60": {"x": 256},
                  "random63": {"buf": 16}}
KNOWN_SOURCE_UB = {
    "random5": "FFT first span=256 iteration reads real[256]/img[256] from 256-element arrays.",
    "random23": "FFT first span=256 iteration reads real[256]/img[256] from 256-element arrays.",
    "random57": "Uninitialized automatic word[4][120] is read before being assigned.",
    "random58": "Uninitialized automatic word[4][120] is read before being assigned.",
    "random66": "Automatic result is read uninitialized for opposite-sign fixed input pairs.",
    "random68": "First bank-zero stencil access has a negative input index for defined log_width shifts.",
}


class Unsupported(RuntimeError):
    pass


class SourceInvalid(RuntimeError):
    pass


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def known_source_failure(source):
    """Keep the original denylist; admit only exact, independently tested repairs."""
    reason = KNOWN_SOURCE_UB.get(source.stem)
    if reason is None:
        return None
    registry = ROOT / "Dataset/Testbenches/source_fixes.cfg"
    if registry.is_file():
        repairs = json.loads(registry.read_text()).get("repairs", {})
        repair = repairs.get(source.stem, {})
        if repair.get("status") == "validated" and repair.get("sha256") == digest(source):
            return None
    return reason


def write_json(path, value):
    path = Path(path)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
    temporary.replace(path)


class Commands:
    """Stay in the supervisor's process group so its hard timeout kills children."""
    def __init__(self, output, timeout):
        self.output, self.timeout, self.records = output, timeout, []

    def run(self, name, command):
        command = [str(x) for x in command]
        log = self.output / (name + ".log")
        record = {"name": name, "command": command, "log": str(log),
                  "timeout_seconds": self.timeout, "status": "running"}
        self.records.append(record)
        started = time.monotonic()
        write_json(self.output / "commands.json", self.records)
        print("RUN " + name, flush=True)
        environment = dict(os.environ)
        # LeakSanitizer cannot run under ptrace; address and UB checks stay on.
        environment["ASAN_OPTIONS"] = "detect_leaks=0"
        with log.open("w") as stream:
            process = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT,
                                       cwd=str(self.output), env=environment)
            record["pid"] = process.pid
            write_json(self.output / "commands.json", self.records)
            try:
                record["exit_code"] = process.wait(timeout=self.timeout)
                record["status"] = "completed" if process.returncode == 0 else "failed"
            except subprocess.TimeoutExpired:
                print("HARD_TIMEOUT " + name + ": terminating command", flush=True)
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                record.update(status="timeout", exit_code=process.returncode)
            record["duration_seconds"] = time.monotonic() - started
        write_json(self.output / "commands.json", self.records)
        text = log.read_text(errors="replace")
        if record["status"] != "completed":
            raise RuntimeError(f"{name}: {record['status']} ({record['exit_code']}); see {log}")
        return text


def integer_expression(text, constants=None):
    """Evaluate only constant integer array expressions from preprocessed source."""
    text = re.sub(r"(?<=\d)[uUlL]+\b", "", text)
    node = ast.parse(text, mode="eval").body
    def evaluate(item):
        if isinstance(item, ast.Constant) and type(item.value) is int:
            return item.value
        if isinstance(item, ast.Name) and item.id in (constants or {}):
            return constants[item.id]
        if isinstance(item, ast.UnaryOp) and isinstance(item.op, (ast.UAdd, ast.USub)):
            value = evaluate(item.operand)
            return value if isinstance(item.op, ast.UAdd) else -value
        if isinstance(item, ast.BinOp):
            left, right = evaluate(item.left), evaluate(item.right)
            if isinstance(item.op, ast.Add): return left + right
            if isinstance(item.op, ast.Sub): return left - right
            if isinstance(item.op, ast.Mult): return left * right
            if isinstance(item.op, (ast.Div, ast.FloorDiv)): return left // right
            if isinstance(item.op, ast.LShift): return left << right
        raise Unsupported("Nonconstant/unmodeled array dimension: " + text)
    value = evaluate(node)
    if value <= 0 or value > 10000000:
        raise Unsupported("Invalid or excessively large array dimension: " + text)
    return value


def parse_interface(preprocessed, ast_text, stem):
    decoder, offset, definitions = json.JSONDecoder(), 0, []
    while offset < len(ast_text):
        offset += len(ast_text[offset:]) - len(ast_text[offset:].lstrip())
        if offset == len(ast_text): break
        item, end = decoder.raw_decode(ast_text, offset)
        offset = end
        if item.get("kind") == "FunctionDecl" and item.get("name") == "dut" and any(
                x.get("kind") == "CompoundStmt" for x in item.get("inner", [])):
            definitions.append(item)
    if len(definitions) != 1:
        raise Unsupported("Expected one defined dut function")
    function = definitions[0]
    return_type = function["type"]["qualType"].split("(", 1)[0].strip()
    aliases = dict((name, base.strip()) for base, name in re.findall(
        r"typedef\s+([\w\s]+?)\s+(\w+)\s*;", preprocessed))
    for _ in range(10):
        if return_type not in aliases: break
        return_type = aliases[return_type]
    if return_type not in ("void", *TYPES):
        raise Unsupported("Unmodeled return type: " + return_type)
    constants = {}
    for name, expression in re.findall(r"\b(?:const|constexpr)\s+(?:unsigned\s+)?(?:int\s+)?(\w+)\s*=\s*([^;]+);", preprocessed):
        try:
            constants[name] = integer_expression(expression, constants)
        except (Unsupported, SyntaxError, ValueError, ZeroDivisionError):
            pass
    arguments = []
    for parameter in function.get("inner", []):
        if parameter.get("kind") != "ParmVarDecl": continue
        name = parameter.get("name")
        if not name or not re.fullmatch(r"[A-Za-z_]\w*", name):
            raise Unsupported("Unnamed or invalid parameter")
        location = parameter["range"]
        start = location["begin"]["offset"]
        end = location["end"]["offset"] + location["end"].get("tokLen", 1)
        declaration = preprocessed[start:end]
        kind = parameter["type"].get("desugaredQualType", parameter["type"]["qualType"])
        base = re.sub(r"\[[^\]]*\]", " ", kind)
        base = re.sub(r"[()*]", " ", base)
        base = " ".join(base.replace("const", "").replace("volatile", "").split())
        for _ in range(10):
            if base not in aliases: break
            base = aliases[base]
        if base in ("signed", "signed int"): base = "int"
        if base not in TYPES:
            raise Unsupported("Unmodeled parameter type " + name + ": " + kind)
        dims = [integer_expression(x, constants) for x in re.findall(r"\[([^\]]+)\]", declaration)]
        pointer = "*" in kind
        if pointer and (not dims or "[]" in declaration.replace(" ", "")):
            if name not in POINTER_DEPTHS.get(stem, {}):
                raise Unsupported("No declared or audited pointer depth for " + name)
            dims = [POINTER_DEPTHS[stem][name]]
        depth = math.prod(dims) if dims else 0
        arguments.append({"name": name, "depth": depth, "dimensions": dims,
                          "const": "const" in kind, "type": kind, "base": base})
    return {"return_type": return_type, "symbol": function.get("mangledName", "dut"),
            "arguments": arguments}


def make_inputs(interface, stem, trial, seed):
    rng = random.Random(seed + trial * 104729)
    def value(index):
        if trial % 4 == 0: return 0
        if trial % 4 == 1: return 1
        if trial % 4 == 2: return index % 3
        return rng.randrange(4)
    inputs = {a["name"]: ([value(i) for i in range(a["depth"])] if a["depth"]
                           else 1 + trial % 3) for a in interface["arguments"]}
    if stem in ("random3", "random31"):
        spec = importlib.util.spec_from_file_location("pilot_verifier", ROOT / "Light_HLS/Tests/DAPOII/verify_rtl.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module.make_case("getTanh" if stem == "random3" else "bnn", trial, seed)
    if stem == "test2":
        inputs["exp"] = 0
        inputs["a"] = [rng.randrange(3) for _ in inputs["a"]]
    if stem == "test7":
        # Keep shifts defined and both source buffers within their eight banks.
        inputs["log_width"] = 3 + trial % 4
        inputs["words_per_image"] = 3
        inputs["wrd"] = trial % 4
    if stem == "random68":
        # The repaired eight-bank kernel accepts widths 8, 16, 32, and 64.
        inputs["log_width"] = 3 + trial % 4
    if stem == "random17":
        inputs["all_zero"] = [[0, 9, 10, 11][(i + trial) % 4] for i in range(len(inputs["all_zero"]))]
    if stem == "random11":
        inputs["el"] = [-3, -1, 0, 2][trial % 4]
    if stem == "random4":
        # g has 128 elements and the source reads g[i * dummy], i in [0, 63].
        inputs["dummy"] = 1 + trial % 2
    if stem == "random28":
        inputs["dlt"] = [0, 1, -1, 2][trial % 4]
    if stem == "random55":
        inputs["min_distances"] = [1000 + i if trial % 2 == 0 else 1 + i
                                   for i in range(len(inputs["min_distances"]))]
    if stem == "random21":
        inputs["A"] = [rng.randrange(3) for _ in inputs["A"]]
        inputs["b"] = [1000 + rng.randrange(24) for _ in inputs["b"]]
    if stem == "random54":
        for name in inputs:
            inputs[name] = [0 if name.startswith("d_") else 1 + rng.randrange(3) for _ in inputs[name]]
    if stem == "random46":
        for name in inputs:
            if name.startswith("array_"):
                bins = [-1, 1, 20, 21, 40, 41, 60, 61, 80, 81]
                inputs[name] = [bins[(i + trial) % len(bins)] for i in range(len(inputs[name]))]
    for argument in interface["arguments"]:
        if argument.get("base") == "bool":
            name = argument["name"]
            inputs[name] = [bool(value) for value in inputs[name]] if argument["depth"] else bool(inputs[name])
    return inputs


def wrapper_source(interface, source=None, symbol=None):
    args = interface["arguments"]
    lines = ["#include <cstdint>", "#include <fstream>", "#include <memory>",
             "static_assert(sizeof(int) == 4, \"32-bit int required\");"]
    if source:
        lines.append("#include " + json.dumps(str(source)))
        called = "dut"
    else:
        types = [a.get("base", "int") + (" *" if a["depth"] else "") for a in args]
        lines.append(interface["return_type"] + " native_dut(" + ", ".join(types) +
                     ") __asm__(" + json.dumps(symbol) + ");")
        called = "native_dut"
    lines += ["int main(int argc, char **argv) {", "  if (argc != 3) return 2;",
              "  std::ifstream input(argv[1], std::ios::binary);",
              "  std::ofstream output(argv[2], std::ios::binary);", "  if (!input || !output) return 3;"]
    expressions = []
    for a in args:
        name, depth, dims = a["name"], a["depth"], a["dimensions"]
        storage = "arg_" + name
        base = a.get("base", "int")
        if depth:
            lines += [f"  std::unique_ptr<{base}[]> {storage}(new {base}[{depth}]);",
                      f"  input.read(reinterpret_cast<char *>({storage}.get()), {depth} * sizeof({base}));"]
            expression = storage + ".get()"
            if source and len(dims) > 1:
                cast = base + " (*)" + "".join(f"[{dim}]" for dim in dims[1:])
                expression = f"reinterpret_cast<{cast}>({expression})"
        else:
            lines += [f"  {base} {storage};", f"  input.read(reinterpret_cast<char *>(&{storage}), sizeof({base}));"]
            expression = storage
        expressions.append(expression)
    lines.append("  if (!input || input.peek() != std::char_traits<char>::eof()) return 4;")
    call = called + "(" + ", ".join(expressions) + ")"
    lines.append("  const " + interface["return_type"] + " result = " + call + ";" if interface["return_type"] != "void" else
                 "  " + call + "; const int result = 0;")
    lines.append("  output.write(reinterpret_cast<const char *>(&result), sizeof(result));")
    for a in args:
        if a["depth"]:
            lines.append(f"  output.write(reinterpret_cast<const char *>(arg_{a['name']}.get()), {a['depth']} * sizeof({a.get('base', 'int')}));")
    lines += ["  return output ? 0 : 5;", "}"]
    return "\n".join(lines) + "\n"


def rtl_testbench(interface, memories, rtl, cases, output, max_cycles):
    spec = importlib.util.spec_from_file_location("pilot_verifier", ROOT / "Light_HLS/Tests/DAPOII/verify_rtl.py")
    old = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(old)
    # DATAFLOW files place task modules before the public dut wrapper.
    top_match = re.search(r"\bmodule\s+dut\s*\(", rtl)
    module, ports = old.parse_ports(rtl[top_match.start():] if top_match else rtl)
    arrays = {a["name"]: a["depth"] for a in interface["arguments"] if a["depth"]}
    arguments = {a["name"]: a for a in interface["arguments"]}
    if not memories and arrays and "module light_hls_pipo_bank" in rtl:
        # DATAFLOW root reports have no root MemoryObjects. Model only explicit
        # unpartitioned ap_memory pins, under this verifier's one-cycle contract.
        memories = []
        for name in arrays:
            width = type_info(arguments[name])[1]
            ordinals = sorted(int(match.group(1)) for port in ports
                              if (match := re.fullmatch(re.escape(name) + r"_address(\d+)", port)))
            if not ordinals or ordinals != list(range(len(ordinals))):
                raise Unsupported("Unmodeled DATAFLOW memory interface: " + name)
            for ordinal in ordinals:
                for suffix in ("q", "d"):
                    pin = f"{name}_{suffix}{ordinal}"
                    if pin in ports and ports[pin][1] != width:
                        raise Unsupported("DATAFLOW memory width mismatch: " + pin)
            memories.append(dict(name=name, storage="external", protocol="ap_memory",
                                 element_width=width, read_latency=1, banks=1, ports=len(ordinals)))
    scalars = {a["name"] for a in interface["arguments"] if not a["depth"]}
    required = {"ap_clk", "ap_rst", "ap_start", "ap_done", "ap_ready", "ap_idle"}
    if interface["return_type"] != "void": required.add("ap_return")
    if not required.issubset(ports): raise Unsupported("Missing control/return ports")
    inputs = {"ap_clk", "ap_rst", "ap_start"}
    scalar_ports = {}
    for name in scalars:
        width = type_info(arguments[name])[1]
        choices = [x for x in (name, name + "_r") if ports.get(x) == ("input", width)]
        if len(choices) != 1: raise Unsupported("Unmodeled scalar RTL port: " + name)
        scalar_ports[name] = choices[0]
        inputs.add(choices[0])
    lines = ["`timescale 1ns/1ps", "module cv_tb;"]
    for name, (direction, width) in ports.items():
        lines.append("  " + ("reg " if direction == "input" else "wire ") +
                     (f"[{width - 1}:0] " if width > 1 else "") + name +
                     (" = 0;" if direction == "input" else ";"))
    for name, depth in arrays.items():
        width = type_info(arguments[name])[1]
        lines += [f"  reg [{width-1}:0] mem_{name}[0:{depth - 1}];",
                  f"  reg [{width-1}:0] expected_{name}[0:{depth - 1}];"]
    # Accept arbitrary quiet-NaN payloads, but never unknown RTL bits.
    for width, fraction in [(16, 10), (32, 23), (64, 52)]:
        exponent_mask = ((1 << (width-fraction-1))-1) << fraction
        fraction_mask = (1 << fraction)-1
        lines += [f"function fp_equal{width}; input [{width-1}:0] a,b; begin",
                  f"fp_equal{width} = (a === b) || ((^a !== 1'bx) && (^b !== 1'bx) &&",
                  f" ((a & {width}'h{exponent_mask:x}) == {width}'h{exponent_mask:x}) &&",
                  f" ((b & {width}'h{exponent_mask:x}) == {width}'h{exponent_mask:x}) &&",
                  f" (|(a & {width}'h{fraction_mask:x})) && (|(b & {width}'h{fraction_mask:x}))); end endfunction"]
    lines += ["  integer tick=0, accepted=-1, last_write=-1, reads=0, writes=0, index, waiting;",
              "  reg active=0;", "  always #5 ap_clk=~ap_clk;",
              "  " + module + " kernel(" + ", ".join(f".{p}({p})" for p in ports) + ");",
              "  always @(posedge ap_clk) begin", "    tick=tick+1;", "    if (!ap_rst) begin",
              "      if (ap_start && ap_ready) begin",
              "        if (active) $fatal(1, \"Duplicate acceptance\");",
              "        accepted=tick; active=1;", "      end"]
    for memory in memories:
        if memory.get("storage") != "external": continue
        name = memory["name"]
        if name not in arrays: raise Unsupported("Unknown memory object: " + name)
        if memory.get("protocol") != "ap_memory" or memory.get("banks", 1) != 1:
            raise Unsupported("Verifier requires unpartitioned ap_memory: " + name)
        if memory.get("element_width") != type_info(arguments[name])[1] or memory.get("read_latency", 1) != 1:
            raise Unsupported("Verifier requires matching scalar-width one-cycle RAM: " + name)
        prefix = memory.get("port_name", name)
        write_ports = []
        for port in range(memory.get("ports", 1)):
            address, ce, we = [f"{prefix}_{suffix}{port}" for suffix in ("address", "ce", "we")]
            data, q = f"{prefix}_d{port}", f"{prefix}_q{port}"
            if ce not in ports or address not in ports: raise Unsupported("Missing memory port: " + prefix)
            if q in ports: inputs.add(q)
            lines += [f"      if ({ce} === 1'bx) $fatal(1, \"Unknown CE {ce}\");",
                      f"      if ({ce}) begin", "        if (!active) $fatal(1, \"Access outside transaction\");",
                      f"        if ((^{address}) === 1'bx || $unsigned({address}) >= {arrays[name]})",
                      f"          $fatal(1, \"Bad address {address}=%h\", {address});"]
            if q in ports: lines.append(f"        {q} <= mem_{name}[{address}];")
            if we in ports:
                if data not in ports: raise Unsupported("Missing memory data: " + prefix)
                write_ports.append((address, ce, we))
                lines += [f"        if ({we} === 1'bx) $fatal(1, \"Unknown WE\");",
                          f"        if ({we}) begin",
                          f"          if ((^{data}) === 1'bx) $fatal(1, \"Unknown write data\");",
                          f"          mem_{name}[{address}] <= {data};",
                          "          writes=writes+1; last_write=tick;",
                          "        end" + (" else reads=reads+1;" if q in ports else "")]
            elif q in ports: lines.append("        reads=reads+1;")
            lines.append("      end")
        if len(write_ports) == 2:
            a0,c0,w0 = write_ports[0]
            a1,c1,w1 = write_ports[1]
            lines += [f"      if ({c0} && {w0} && {c1} && {w1} && {a0} == {a1})",
                      "        $fatal(1, \"Conflicting dual writes\");"]
    unknown = {name for name, (direction, _) in ports.items() if direction == "input"} - inputs
    if unknown: raise Unsupported("Unmodeled input ports: " + str(sorted(unknown)))
    lines += ["    end", "  end", "  initial begin", "    ap_rst=1;",
              "    repeat(3) @(negedge ap_clk);", "    ap_rst=0;"]
    for trial, case in enumerate(cases):
        if trial and trial % 2 == 0:
            lines += ["    ap_rst=1; repeat(3) @(negedge ap_clk); ap_rst=0;"]
        directory = output / f"case_{trial:03d}"
        for name in arrays:
            lines += [f"    $readmemh({json.dumps(str(directory / ('input_' + name + '.hex')))}, mem_{name});",
                      f"    $readmemh({json.dumps(str(directory / ('expected_' + name + '.hex')))}, expected_{name});"]
        for name, port in scalar_ports.items():
            width = type_info(arguments[name])[1]
            bits = bit_value(case['inputs'][name], arguments[name].get('base', 'int'))
            lines.append(f"    {port}={width}'h{bits:x};")
        lines += ["    accepted=-1; last_write=-1; reads=0; writes=0; waiting=0; ap_start=1;",
                  "    while(accepted<0 && waiting<100) begin @(negedge ap_clk); waiting=waiting+1; end",
                  "    if(accepted<0) $fatal(1, \"No acceptance\");", "    ap_start=0;"]
        for port in scalar_ports.values(): lines.append(f"    {port}=~{port};")
        lines += [f"    while(!ap_done && tick-accepted<{max_cycles}) @(negedge ap_clk);",
                  "    if(!ap_done) $fatal(1, \"Transaction timeout\");"]
        if interface["return_type"] != "void":
            base = interface["return_type"]
            width = TYPES[base][1]
            expected = f"{width}'h{case['return'] & ((1 << width)-1):x}"
            condition = (f"!fp_equal{width}(ap_return, {expected})" if base in ("_Float16", "float", "double") else
                         f"ap_return !== {expected}")
            lines.append(f"    if({condition}) $fatal(1, \"Return mismatch\");")
        for name, depth in arrays.items():
            base = arguments[name].get("base", "int")
            condition = (f"!fp_equal{type_info(arguments[name])[1]}(mem_{name}[index], expected_{name}[index])"
                         if base in ("_Float16", "float", "double") else f"mem_{name}[index] !== expected_{name}[index]")
            lines += [f"    for(index=0;index<{depth};index=index+1)",
                      f"      if({condition})",
                      f"        $fatal(1, \"Memory mismatch {name}[%0d]\", index);"]
        lines += [f"    $display(\"RESULT trial={trial} cycles=%0d last_write_cycle=%0d reads=%0d writes=%0d\", tick-accepted, last_write<0 ? -1 : last_write-accepted, reads, writes);",
                  "    active=0; repeat(3) @(negedge ap_clk);"]
    lines += [f"    $display(\"CV RTL VERIFICATION PASSED: {len(cases)} cases\");", "    $finish;", "  end", "endmodule"]
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("source", "output"):
        parser.add_argument("--" + option, type=Path, required=True)
    parser.add_argument("--reports", type=Path)
    parser.add_argument("--ir", type=Path)
    parser.add_argument("--cases", type=int, default=4)
    parser.add_argument("--seed", type=int, default=17)
    parser.add_argument("--timeout-seconds", type=int, default=300)
    parser.add_argument("--max-cycles", type=int, default=5000000)
    parser.add_argument("--reference-only", action="store_true")
    args = parser.parse_args()
    if min(args.cases, args.timeout_seconds, args.max_cycles) <= 0: parser.error("Positive limits required")
    if not args.reference_only and not args.reports: parser.error("--reports is required")
    source, output = args.source.resolve(strict=True), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if (output / "verification.json").exists(): parser.error("Refusing to overwrite prior verification")
    result = {"schema_version": "cv_verification_v1", "source": str(source), "source_sha256": digest(source),
              "status": "running", "rtl_correct": None,
              "native_ir_equivalence": "pending" if args.ir else "not_requested", "cases": [],
              "seed": args.seed, "requested_cases": args.cases, "reference_asan_ubsan": False,
              "sanitizer_options": {"ASAN_OPTIONS": "detect_leaks=0", "reason": "LeakSanitizer is incompatible with ptrace; address and undefined-behavior checks remain enabled."},
              "coverage": "Four bounded deterministic input patterns, with audited kernel-specific domain constraints. No exhaustive equivalence claim; no cross-argument aliasing; ASan/UBSan do not prove absence of all UB.",
              "latency_definition": "Rising edges from accepted ap_start&&ap_ready to observed ap_done; final memory write reported separately.",
              "memory_contract": "Independent typed scalar arrays, one-cycle read-first RAM, same-address simultaneous writes rejected; NaN payloads may differ, all other bits must match.",
              "reset_protocol": "Warm second call; reset between subsequent pairs; no reset-abort coverage."}
    commands = Commands(output, args.timeout_seconds)
    write_json(output / "verification.json", result)
    phase = "interface"
    try:
        source_failure = known_source_failure(source)
        if source_failure: raise SourceInvalid(source_failure)
        clang = shutil.which("clang++")
        if not clang: raise RuntimeError("clang++ not available")
        preprocessed = output / "source.ii"
        commands.run("preprocess", [clang, "-E", "-P", "-std=c++17", source, "-o", preprocessed])
        ast_text = commands.run("interface_ast", [clang, "-std=c++17", "-Xclang", "-ast-dump=json",
                                 "-Xclang", "-ast-dump-filter=dut", "-fsyntax-only", preprocessed])
        interface = parse_interface(preprocessed.read_text(), ast_text, source.stem)
        result["interface"] = interface
        write_json(output / "interface.json", interface)
        wrapper = output / "source_reference.cpp"
        wrapper.write_text(wrapper_source(interface, source=source))
        oracle = output / "source_reference"
        phase = "source_compile"
        commands.run(phase, [clang, "-std=c++17", "-O0", "-g", "-Wno-unknown-pragmas",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=all", wrapper, "-o", oracle])
        candidate = None
        if args.ir:
            ir = args.ir.resolve(strict=True)
            result.update(ir=str(ir), ir_sha256=digest(ir))
            ir_text = ir.read_text() if ir.suffix == ".ll" else commands.run("disassemble", ["llvm-dis", ir, "-o", "-"])
            symbols = [a or b for a,b in re.findall(r'^define\b[^@\n]*@(?:"([^"]+)"|([\w.$-]+))\(', ir_text, re.M)
                       if (a or b) == "dut" or (a or b).startswith("_Z3dut")]
            if len(symbols) != 1: raise Unsupported("Cannot identify candidate dut symbol")
            wrapper_ir = output / "candidate_reference.cpp"
            wrapper_ir.write_text(wrapper_source(interface, symbol=symbols[0]))
            candidate = output / "candidate_reference"
            commands.run("ir_compile", [clang, "-std=c++17", "-O0", "-Wno-override-module", ir, wrapper_ir, "-o", candidate])
        payloads = []
        for trial in range(args.cases):
            values = make_inputs(interface, source.stem, trial, args.seed)
            directory = output / f"case_{trial:03d}"
            directory.mkdir()
            packed = bytearray()
            for a in interface["arguments"]:
                value = values[a["name"]]
                if a["depth"]:
                    if len(value) != a["depth"]: raise RuntimeError("Fixture depth mismatch")
                    (directory / ("input_" + a["name"] + ".hex")).write_text("".join(f"{bit_value(x, a['base']):x}\n" for x in value))
                for item in (value if a["depth"] else [value]):
                    packed += encode_value(item, a["base"])
            input_path = directory / "input.bin"
            input_path.write_bytes(packed)
            reference_path = directory / "reference.bin"
            phase = "source_reference"
            commands.run(f"source_case_{trial}", [oracle, input_path, reference_path])
            raw = reference_path.read_bytes()
            return_base = interface["return_type"] if interface["return_type"] != "void" else "int"
            word_types = [return_base]
            for a in interface["arguments"]:
                word_types += [a["base"]] * a["depth"]
            expected_size = sum(struct.calcsize(TYPES[base][0]) for base in word_types)
            if len(raw) != expected_size: raise RuntimeError("Invalid oracle output size")
            expected, offset = [], 0
            for base in word_types:
                size = struct.calcsize(TYPES[base][0])
                expected.append(int.from_bytes(raw[offset:offset+size], "little"))
                offset += size
            cursor = 1
            for a in interface["arguments"]:
                if a["depth"]:
                    words = expected[cursor:cursor + a["depth"]]
                    (directory / ("expected_" + a["name"] + ".hex")).write_text("".join(f"{x:x}\n" for x in words))
                    cursor += a["depth"]
            if candidate:
                phase = "native_ir"
                native_path = directory / "candidate.bin"
                commands.run(f"native_case_{trial}", [candidate, input_path, native_path])
                native, offset = native_path.read_bytes(), 0
                if len(native) != len(raw): raise RuntimeError("Native output size mismatch")
                for base in word_types:
                    size = struct.calcsize(TYPES[base][0])
                    if not equal_word(native[offset:offset+size], raw[offset:offset+size], base):
                        raise RuntimeError("Native optimized IR/source mismatch")
                    offset += size
            payloads.append({"inputs": values, "return": expected[0]})
            result["cases"].append({"trial": trial, "input_sha256": digest(input_path),
                                    "reference_sha256": digest(reference_path), "return": expected[0]})
            write_json(output / "verification.json", result)
        result["reference_asan_ubsan"] = True
        if candidate: result["native_ir_equivalence"] = "passed"
        if not args.reference_only:
            phase = "rtl_testbench"
            reports = args.reports.resolve(strict=True)
            rtl = reports / "rtl/top.v"
            spec = importlib.util.spec_from_file_location("pilot_verifier", ROOT / "Light_HLS/Tests/DAPOII/verify_rtl.py")
            old = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(old)
            bench = output / "testbench.v"
            bench.write_text(rtl_testbench(interface, old.read_memories(reports), rtl.read_text(), payloads, output, args.max_cycles))
            result["rtl_sha256"] = digest(rtl)
            result["rtl_correct"] = False
            phase = "rtl_compile"
            executable = output / "rtl_simulation"
            commands.run(phase, ["iverilog", "-g2012", "-Wall", "-s", "cv_tb", "-o", executable, rtl, bench])
            phase = "rtl_simulation"
            log = commands.run(phase, ["vvp", executable])
            rows = re.findall(r"^RESULT trial=(\d+) cycles=(\d+) last_write_cycle=(-?\d+) reads=(\d+) writes=(\d+)$", log, re.M)
            if len(rows) != args.cases or "CV RTL VERIFICATION PASSED" not in log:
                raise RuntimeError("Incomplete simulation results")
            for entry, row in zip(result["cases"], rows):
                trial, cycles, last_write, reads, writes = map(int, row)
                if trial != entry["trial"]: raise RuntimeError("Unexpected trial order")
                entry.update(cycles=cycles, last_write_cycle=last_write, reads=reads, writes=writes)
            result["rtl_correct"] = True
        if result["source_sha256"] != digest(source): raise RuntimeError("Source changed during verification")
        if args.ir and result["ir_sha256"] != digest(args.ir): raise RuntimeError("IR changed during verification")
        result["status"] = "passed"
    except (OSError, RuntimeError, ValueError, KeyError) as error:
        status = "unsupported" if isinstance(error, Unsupported) else "source_invalid" if isinstance(error, SourceInvalid) else "failed"
        if phase == "source_reference": status = "source_reference_failed"
        result.update(status=status, failed_phase=phase, error=str(error))
        if result["native_ir_equivalence"] == "pending": result["native_ir_equivalence"] = "not_completed"
        print(str(error), file=sys.stderr)
    write_json(output / "verification.json", result)
    print("VERIFICATION " + result["status"] + ": " + str(output / "verification.json"), flush=True)
    return 0 if result["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
