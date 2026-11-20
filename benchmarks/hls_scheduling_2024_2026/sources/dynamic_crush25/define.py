from pathlib import Path

# -- paths -- #
BENCHMARK_SRC_DIR = Path("./benchmarks").absolute()

FAST_BENCHMARK_SRC_DIR = Path("./benchmarks-fast-token").absolute()

LEGACY_COMPILE_SCRIPT = Path("./flow_compile_legacy.sh").absolute()

LEGACY_COMPILE_FAST_SCRIPT = Path("./flow_compile_fast.sh").absolute()

SHARING_SCRIPT = Path("./flow_sharing.py").absolute()

SHARING_SCRIPT_MANUAL = Path("./flow_sharing_manual.py").absolute()

LEGACY_DYNAMATIC_DIR = Path("./legacy-dynamatic").absolute()

LEGACY_FAST_DIR = Path("./legacy-dynamatic-fast-token").absolute()


LEGACY_DOT2VHDL_BIN = (
    Path("./legacy-dynamatic")
    / "etc"
    / "dhls"
    / "dot2vhdl"
    / "bin"
    / "dot2vhdl"
).absolute()

LEGACY_BUFFER_BIN = (
    Path("./legacy-dynamatic") / "etc" / "dhls" / "Buffers" / "bin" / "buffers"
).absolute()

FAST_BUFFER_BIN = (
    LEGACY_FAST_DIR / "etc" / "dynamatic" / "Buffers" / "bin" / "buffers"
).absolute()


FCCM_SHARING_BIN = (
    LEGACY_DYNAMATIC_DIR
    / "etc"
    / "dhls"
    / "resource_sharing"
    / "bin"
    / "ResourceSharing"
)

SIMULATE_SCRIPT = Path("./flow_simulate.sh").absolute()

SYNTHESIS_SCRIPT = Path("./flow_synthesize.sh").absolute()

STEPS = ("compile",)
STEPS = ("sharing",)
STEPS = ("compile", "sharing")

# -- benchmarks -- #

MILP_TIMEOUT = (
    120  # 120 s or 2 min (for buffer placement / performance estimation)
)

LOSS_FACTOR = 1.0

CLOCK_PERIOD = 6.0


# -- benchmarks -- #

BENCHMARKS = (
    "atax_float",
    "bicg_float",
    "gemm_float",
    "gesummv_float",
    "symm_float",
    "gsum",
    "gsumif",
    "kernel_2mm_float",
    "kernel_3mm_float",
    "mvt_float",
    "syr2k_float",
)

FAST_BENCHMARKS = (
    "atax_float",
    "bicg_float",
    "gemm_float",
    "gesummv_float",
    "gsum",
    "kernel_2mm_float",
    "kernel_3mm_float",
    "mvt_float",
    "symm_float",
    "syr2k_float",
)
