#!/usr/bin/env python3

from pathlib import Path
from utils import *
from time import perf_counter

from define import (
    BENCHMARKS,
    FAST_BENCHMARKS,
    FAST_BENCHMARK_SRC_DIR,
    LEGACY_COMPILE_SCRIPT,
    LEGACY_COMPILE_FAST_SCRIPT,
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
    LEGACY_FAST_DIR,
    FAST_BUFFER_BIN,
)

BENCHMARK_DIR = Path("./e5_fast_token").absolute()


def main():
    for bm in FAST_BENCHMARKS:
        # make sure that we are using the same c "src"
        run(["rm", "-rf", BENCHMARK_DIR / bm / "src"])
        run(["mkdir", "-p", BENCHMARK_DIR / bm])
        run(
            [
                "cp",
                "-r",
                FAST_BENCHMARK_SRC_DIR / bm / "src",
                BENCHMARK_DIR / bm / "src",
            ]
        )
        dbenchmark = BENCHMARK_DIR / bm
        if "compile" in STEPS:
            run(
                [
                    "bash",
                    LEGACY_COMPILE_FAST_SCRIPT,
                    LEGACY_FAST_DIR,
                    dbenchmark,
                    bm + ".cpp",
                ],
                cwd=BENCHMARK_DIR / bm,
            )
        if "sharing" in STEPS:
            start_time = perf_counter()
            # run performance analysis, save the raw output in a logfile
            f_buffer_log = BENCHMARK_DIR / bm / "reports" / "buffers.log"
            with open(f_buffer_log, "w") as f:
                f.write(
                    run(
                        [
                            FAST_BUFFER_BIN,
                            "buffers",
                            "-filename="
                            + str(BENCHMARK_DIR / bm / "reports" / bm),
                            "-period=6.0",
                            "-solver=gurobi_cl",
                            "-timeout=" + str(MILP_TIMEOUT),
                        ],
                        stdout=subprocess.PIPE,
                        universal_newlines=True,
                        cwd=BENCHMARK_DIR / bm,
                    ).stdout
                )
            milp_time = perf_counter() - start_time
            run(
                [
                    "cp",
                    BENCHMARK_DIR / bm / "reports" / (bm + "_graph_buf.dot"),
                    BENCHMARK_DIR
                    / bm
                    / "reports"
                    / (bm + "_area_optimized.dot"),
                ],
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
            fdot = dbenchmark / "reports" / (bm + "_area_optimized.dot")

            run(
                ["bash", SIMULATE_SCRIPT, LEGACY_DYNAMATIC_DIR, fdot, bm],
                cwd=BENCHMARK_DIR / bm,
            )

            run(["bash", SYNTHESIS_SCRIPT, dbenchmark])


if __name__ == "__main__":
    main()
