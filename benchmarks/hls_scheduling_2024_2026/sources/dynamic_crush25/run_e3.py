#!/usr/bin/env python3

from pathlib import Path
from utils import *
from time import perf_counter

from define import (
    BENCHMARKS,
    BENCHMARK_SRC_DIR,
    LEGACY_COMPILE_SCRIPT,
    SHARING_SCRIPT,
    LEGACY_DYNAMATIC_DIR,
    LEGACY_BUFFER_BIN,
    FCCM_SHARING_BIN,
    SIMULATE_SCRIPT,
    SYNTHESIS_SCRIPT,
    STEPS,
    MILP_TIMEOUT,
    LOSS_FACTOR,
    CLOCK_PERIOD,
)

BENCHMARK_DIR = Path("./e3_fpga18").absolute()


def main():
    for bm in BENCHMARKS:
        # make sure that we are using the same c "src"
        run(["rm", "-rf", BENCHMARK_DIR / bm / "src"])
        run(["mkdir", "-p", BENCHMARK_DIR / bm])
        run(
            [
                "cp",
                "-r",
                BENCHMARK_SRC_DIR / bm / "src",
                BENCHMARK_DIR / bm / "src",
            ]
        )
        dbenchmark = BENCHMARK_DIR / bm
        if "compile" in STEPS:
            run(
                [
                    "bash",
                    LEGACY_COMPILE_SCRIPT,
                    LEGACY_DYNAMATIC_DIR,
                    dbenchmark,
                    bm + ".cpp",
                ],
                cwd=BENCHMARK_DIR / bm,
            )

        if "sharing" in STEPS:

            start_time = perf_counter()
            cmd = [
                LEGACY_BUFFER_BIN,
                "buffers",
                "-filename=" + str(BENCHMARK_DIR / bm / "reports" / (bm)),
                "-period=6.0",
                "-solver=gurobi_cl",
                "-timeout=" + str(MILP_TIMEOUT),
            ]
            run(
                cmd,
                cwd=BENCHMARK_DIR / bm,
            )
            with open(
                BENCHMARK_DIR / bm / "reports" / ("resource_sharing.log"), "w"
            ) as f:
                f.write(
                    "total time: "
                    + "{0:.2f}".format(perf_counter() - start_time)
                    + "s"
                )

            # generate the vhd netlist and run simulation
            fdot = dbenchmark / "reports" / (bm + "_graph_buf.dot")
            fdot = fdot.rename(
                dbenchmark / "reports" / (bm + "_area_optimized.dot")
            )

            cmd = ["bash", SIMULATE_SCRIPT, LEGACY_DYNAMATIC_DIR, fdot, bm]

            run(
                cmd,
                cwd=BENCHMARK_DIR / bm,
            )

            cmd = ["bash", SYNTHESIS_SCRIPT, dbenchmark]

            run(cmd)


if __name__ == "__main__":
    main()
