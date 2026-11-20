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

BENCHMARK_DIR = Path("./e2_fccm22").absolute()


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

            loss = LOSS_FACTOR

            start_time = perf_counter()
            cmd = [
                FCCM_SHARING_BIN,
                "-filename="
                + str(BENCHMARK_DIR / bm / "reports" / (bm + ".dot")),
                "-model_mode=default",  # use FPGA '20 buffers
                "-period=6.0",
                "-max_loss=" + "{:.1f}".format(loss),
                "-timeout=" + str(MILP_TIMEOUT),
            ]
            run(
                cmd,
                cwd=BENCHMARK_DIR / bm,
            )
            execution_time = perf_counter() - start_time
            sharing_log = Path(
                BENCHMARK_DIR / bm / "reports" / ("resource_sharing.log")
            ).absolute()
            with open(sharing_log, "w") as f:
                f.write("total time: " + "{0:.2f}".format(execution_time) + "s")

            fdot = dbenchmark / "reports" / (bm + "_area_optimized.dot")

            # HACK: fixing the netlist using an external script
            run(["python3", "./fix_dot.py", fdot])

            cmd = ["bash", SIMULATE_SCRIPT, LEGACY_DYNAMATIC_DIR, fdot, bm]

            run(
                cmd,
                cwd=BENCHMARK_DIR / bm,
            )

            # copy the generated files
            cmd = ["bash", SYNTHESIS_SCRIPT, dbenchmark]

            run(cmd)


if __name__ == "__main__":
    main()
