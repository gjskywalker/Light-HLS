#!/usr/bin/env python3
"""Compare scheduled FP pipeline/recurrence estimates with source and RTL."""
import argparse
import json
from pathlib import Path
import sys
from verify_floating import run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    binary = args.binary.resolve()
    results = []
    for width, kind in [(16, "_Float16"), (32, "float"), (64, "double")]:
        for kernel in ("pipeline", "recurrence", "fma_recurrence"):
            directory = output / (kernel + str(width))
            directory.mkdir()
            source = directory / "source.cpp"
            text = (root / f"Light_HLS/App/backend_floating/backend_fp_{kernel}.cpp").read_text().replace("float", kind)
            if width == 64: text = text.replace("__builtin_sqrtf", "__builtin_sqrt")
            source.write_text(text)
            common = ["--board", "u250", "--freq", "100MHz"]
            prepared = directory / "prepared"
            run([binary, "--prepare-source", source, "dut", *common, "--output", prepared], directory, "prepare")
            manifest = prepared / "reports/dapo_manifest.json"
            ir = json.loads(manifest.read_text())["ir"]
            generated = directory / "generated"
            run([binary, ir, "dut", "--manifest", manifest, *common, "--latency-model", "scheduled", "--output", generated], directory, "generate")
            run([sys.executable, root / "experiments/dapo_ii_cross_validation/verify_design.py", "--source", source,
                 "--ir", ir, "--reports", generated / "reports", "--output", directory / "verification", "--cases", "4"], directory, "verify")
            estimate = json.loads((generated / "reports/estimate/dapo_estimate.json").read_text())
            verified = json.loads((directory / "verification/verification.json").read_text())
            measured = [row["cycles"] for row in verified["cases"]]
            expected = estimate["scheduled_latency"]["cycles"]
            passed = verified["status"] == "passed" and expected["constant"] and all(c == expected["maximum"] for c in measured)
            results.append(dict(width=width, kernel=kernel, passed=passed, measured_cycles=measured,
                                estimated_cycles=expected, resources=estimate.get("floating_operator_resources")))
            (output / "results.json").write_text(json.dumps(results, indent=2)+"\n")
            print(width, kernel, measured, expected, passed, flush=True)
            if not passed: raise SystemExit(1)


if __name__ == "__main__": main()
