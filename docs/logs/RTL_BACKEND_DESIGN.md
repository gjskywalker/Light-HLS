# Light-HLS scheduling, binding, and RTL backend

## Purpose and stage boundaries

The RTL backend is an independent pipeline after
`HI_WithDirectiveTimingResourceEvaluation`. The existing evaluator remains a
pre-backend heuristic cost model. Its latency and resource predictions are
copied into `PreBackendEstimate`, but its internal `Inst_Schedule` is not treated
as the final schedule or binding.

The backend also imports per-instruction cycle latency and combinational delay,
including DATAFLOW child functions. These values guide clock-budgeted chaining,
not the final FSM placement. Proven continuous two-loop nests have separate
source and hardware trip counts; both the scheduler and DATAFLOW performance
analysis follow the hardware exits. See
[the clock-aware optimization notes](QOR_OPTIMIZATION_20260908.md) for the
proof restrictions, register/mux changes, and measured U250 results.

The backend pipeline is:

```text
HI_WithDirectiveTimingResourceEvaluation
  -> HI_BackendValidation (pre-backend capability check)
  -> HI_MemoryObjectAnalysis
  -> HI_LoopPipelineAnalysis
  -> HI_ArrayPartitionAnalysis
  -> HI_StorageBindingAnalysis
  -> HI_BackendDependenceAnalysis
  -> HI_OperationScheduling
  -> HI_PipelineStorageAnalysis
  -> HI_ResourceBinding
  -> HI_RTLGeneration
  -> HI_BackendValidation (post-RTL consistency check)
  -> HI_VerilogEmission
  -> HI_RTLSynthesis (optional Vivado measurement)
```

All backend passes share one `HLSBackendContext`. No pass reads another pass's
private members, and no backend state is stored in global variables.

When the top function contains a supported `DATAFLOW` region, the backend uses
the task-level branch after evaluation:

```text
HI_WithDirectiveTimingResourceEvaluation
  -> HI_DataflowAnalysis
  -> HI_DataflowTaskGeneration
       -> regular memory/scheduling/binding/RTL passes per task
  -> HI_DataflowPerformanceAnalysis
  -> HI_DataflowRTLGeneration
  -> HI_BackendValidation (post-RTL consistency check)
  -> HI_VerilogEmission
  -> HI_RTLSynthesis (optional Vivado measurement)
```

## Current supported subset

The first implementation supports:

- integer scalar arguments and integer or void return values;
- integer arithmetic, shifts, bitwise operators, comparisons, casts, and select;
- branches, sequential loops, and PHI nodes;
- fixed-trip canonical single-block loop pipelines with integer datapaths;
- an FSM controller and registered SSA values;
- top-level integer pointer arguments lowered to independent one- or two-port
  direction-aware `ap_memory` interfaces;
- fixed-size local integer arrays lowered to internal synchronous RAMs;
- cyclic, block, and complete array partitioning for external and local arrays,
  with one-dimensional, multidimensional, selected-dimension, and `dim=0`
  layouts;
- exact memory depths recovered from source array parameters or supplied by an
  explicit backend configuration, plus conservative minimum-depth inference
  for pointer parameters with statically bounded IR accesses;
- non-volatile integer loads and stores with configurable synchronous memory
  read latency;
- explicit local-memory `BIND_STORAGE` selection for `ram_1p` and `ram_2p`,
  BRAM, URAM, LUTRAM, and a positive synchronous read latency;
- direct-call task-level DATAFLOW regions with one generated RTL submodule per
  task, persistent bounded task queues, bounded FIFO backpressure, scalar
  streams, arrays of streams, one-producer/many-consumer FIFO broadcast,
  top-level scalar `ap_fifo` ports, and two-buffer local-array PIPO channels;
- composition of task-local PIPELINE with cyclic, block, or complete
  ARRAY_PARTITION on external memories and internal PIPO channels;
- exact partial PIPO transfers, initialized FIFO tokens, and static or bounded
  DATAFLOW rates with balance, capacity, zero-token-cycle, and steady-state
  graph-II analysis;
- a Vitis-like `ap_ctrl_hs` interface (`ap_start`, `ap_done`, `ap_idle`, and
  `ap_ready`);
- synthesizable Verilog-2001 accepted by Icarus Verilog.

PHI nodes are lowered on CFG edges. All PHI assignments belonging to one edge
use nonblocking assignments, preserving LLVM's parallel PHI semantics. A loop
back edge therefore uses the same mechanism as any other transition.

### Transaction acceptance contract

For ordinary and loop-pipelined generated modules, scalar arguments and entry
PHIs are sampled on the rising edge of `ap_start && ap_ready`, outside reset
and, for task modules, only when `ap_ce` is enabled. `ap_ready` is combinational
from the idle FSM state (and reset/clock enable); it is not a delayed completion
acknowledgement. `ap_done` remains a separate completion indication. A held
`ap_start` may offer the next transaction immediately; arguments may change
after the acceptance edge. Single-cycle starts in an accepting idle state
remain supported. Busy unaccepted pulses are not queued by ordinary modules.
Ready is not a memory-lifetime fence: an input RAM must remain valid for all
scheduled reads, and an output RAM must not be reused before the transaction's
final memory commit. Changing captured scalar ports does not imply that a
shared RAM can also be overwritten immediately.

This is the Light-HLS acceptance contract, not a promise to reproduce Vitis's
cycle-by-cycle `ap_ready` waveform. DATAFLOW tops have a two-entry transaction
queue and already acknowledge their admission event; kernel equivalence is
checked using independent result commits, not equal ready/done timing.

Reset aborts active computation and suppresses memory request/valid events.
It does not roll back writes committed before reset or initialize every RAM
location. DATAFLOW task memory ports expose raw requests to the parent. The
parent derives task clock enables from those requests and gates actual FIFO,
PIPO and external-memory commits; gating the raw request with that same enable
would introduce a combinational cycle.

The current scheduler performs basic-block-local ASAP scheduling from SSA data
dependencies. Values crossing a basic-block boundary are registered. The final
control edge may coincide with the last side effect, provided branch, return,
and successor-PHI operands are already ready at that edge. Operation latency
comes from the evaluator when available and otherwise defaults to one cycle.
Eligible one-cycle, statically banked RAM responses are forwarded directly
when every consumer samples at the response-ready edge; other loads retain
explicit capture. The same proof is applied inside pipelined regions.

`HI_LoopPipelineAnalysis` maps source loop labels onto the final LLVM loop
structure before scheduling. The initial pipeline implementation requires one
canonical loop block, a preheader, one latch, one exit, and a positive
compile-time trip count. The scheduler computes memory and recurrence lower
bounds, performs modulo memory-port reservation, and increases the requested II
when necessary. `pipeline_schedule.rpt` and `pipeline_schedule.json` report the
requested and achieved II, RecMII, MemMII, dependence MII, pipeline latency,
stage count, trip count, and limiting factor.

The RTL pipeline engine maintains per-token context registers and valid/slot
shift state for fill, steady-state launch, and drain. Context allocation is
backward-sliced from stores, terminal return values, exit PHIs, memory
addresses, and exact recurrence feedback. LLVM values used only by the
original loop branch are not materialized because the hardware trip-count
controller replaces that branch. Live affine induction PHIs share one compact
per-slot iteration index and are reconstructed at their uses instead of each
allocating a full-width context array. Unused pipeline-global copies are
omitted.
`HI_PipelineStorageAnalysis` then sizes each stored value from its write and
last sampling edge. A smaller ring must divide the controller ring size;
one-slot values become scalar stage registers. Proven II=1 regions with only
one-slot values use fixed-stage iteration tags instead of dynamically indexed
iteration-context arrays. Other regions retain the ring controller. See
[the storage optimization notes](RTL_STORAGE_OPTIMIZATION_20260911.md) for
feedback, retirement, and backpressure correctness contracts.
The storage pass may enlarge the controller ring to the next power of two
when this enables smaller divisor-sized value buffers and saves more payload
bits than it adds iteration-index bits. This is a structural heuristic, not a
physical FF/LUTRAM cost model. A retirement-only distance-one recurrence result
can alias its one-slot feedback PHI when both receive the same typed value on
the same edge; the PHI alone retains initialization.
The controller derives terminal-token identity from the launch count and ring
slot, so it does not require separate active, last-token, or previous-slot
registers; an II=1 pipeline also omits the phase counter. Unconditional
preheader transitions and terminal return blocks are folded into adjacent
states when registered-value timing permits it. Affine induction PHIs are
generated directly from the launch index. Exact distance-one scalar PHI
recurrences are represented as typed dependence edges. Their producer results
are forwarded into the next iteration context, and RecMII is computed from the
producer completion and consumer start cycles. A scalar recurrence that cannot
be expressed exactly retains the conservative completed-iteration fallback.

Scheduling records proven hardware constants without rewriting LLVM IR.
Only recovered memory-object roots become zero object-relative address bases;
dependent integer expressions are folded only when all operands are known.
An otherwise empty function entry may jump directly to its pipeline. If all
stored PHI initial values are constants, initialization also moves onto the
start-acceptance edge; runtime seeds retain header initialization after scalar
arguments have been captured. A final store may share the retirement edge,
while exported registered results must already be available for sampling.
DATAFLOW access offsets and structural latency use the selected entry mode.
See [the boundary optimization notes](RTL_BOUNDARY_OPTIMIZATION_20260914.md).

`HI_BackendDependenceAnalysis` extracts an affine element address `a*i+b` for
each pipeline load and store using ScalarEvolution and the recovered
`MemoryObject` base. It proves disjoint ranges immediately and otherwise checks
the finite loop domain for equal addresses and the minimum legal dependence
distance. The resulting typed RAW, WAR, and WAW edges constrain both
same-iteration scheduling and dependence MII. Non-affine addresses, arithmetic
overflow, or trip counts beyond the exact enumeration limit remain legal but
use conservative non-overlapping iteration scheduling. The human and JSON
pipeline reports expose every scalar recurrence, affine access, memory edge,
proof status, distance, and fallback reason.

`HI_ArrayPartitionAnalysis` resolves source dimensions before dependence
analysis. A Vitis dimension is one-based; `dim=0` expands the directive across
every dimension. Cyclic banking maps coordinate `c` to `c % factor`, block
banking maps it to `c / ceil(extent/factor)`, and complete partitioning uses
one bank per coordinate. Multidimensional bank numbers and bank-local addresses
are composed in row-major order. Non-divisible layouts use a padded rectangular
bank depth, while accesses remain bounded by the original array depth.

Each memory access records either an exact possible-bank set or the complete
bank set when its address cannot be resolved safely. Disjoint exact bank sets
prove that a memory dependence does not exist. The scheduler reserves ports per
bank, the binder assigns a port valid for every possible runtime bank, and RTL
generation emits the same bank and local-address functions. This keeps
dependence analysis, MemMII, binding, and generated hardware consistent.

Partially partitioned external arrays expose one `ap_memory` interface per
bank, using Vitis names such as `input_0_address0`. When every element is its
own bank, Vitis scalarizes the array; Light-HLS therefore emits `input_0`,
`input_1`, and so on for read-only elements, with `ap_vld` output forms for
writable elements and Vitis-compatible `_i`, `_o`, `_o_ap_vld` `ap_ovld`
forms for read/write elements. Local partitioned arrays become independent
internal bank storage arrays and never add top-level ports.

For nested loops, exactly one loop in an ancestor/descendant chain may carry a
PIPELINE directive. Pipelining the innermost loop preserves sequential outer
loop control. Pipelining an outer loop first fully unrolls every descendant
loop, matching the supported Vitis-style structural interpretation. Multiple
PIPELINE directives on nested loops are rejected with
`BACKEND_UNSUPPORTED_NESTED_PIPELINES`; independent sibling loops may be
pipelined separately.

Each pointer argument becomes an external `MemoryObject`. Memory analysis
classifies the whole object as `read_only`, `write_only`, `read_write`, or
`unused`. Resource binding then records read/write usage independently for each
assigned physical port. RTL generation emits only the signals required by that
bound port:

| Bound port access | Emitted signals |
|---|---|
| Read only | `address`, `ce`, `q` |
| Write only | `address`, `ce`, `we`, `d` |
| Read/write | `address`, `ce`, `we`, `d`, `q` |
| Unused external port | none |

A two-port object uses the same rule independently for port ordinals `0` and
`1`. Top-level source names that are Verilog keywords receive the Vitis-style
`_r` suffix, so parameters named `input` and `output` produce `input_r_*` and
`output_r_*` signals. Scalar top ports use the same naming rule. Internal names
continue to use the backend's `_v` keyword suffix and therefore remain separate
from the external compatibility contract.

A static integer `alloca`, including a nested fixed-size array, becomes a local
`MemoryObject` backed by an internal Verilog array and does not appear at the
top-level RTL interface. Local RAMs retain their complete internal signal set
on every configured port, including an otherwise unused port, so future storage
mapping can change without altering top-level RTL compatibility.

The scheduler enforces the configured one- or two-request capacity for each
memory object in each cycle. A request that exceeds that capacity is moved to
the next available cycle. `schedule.json` reports both the number of such
conflicts and the total serialization cycles. The binder assigns concurrent
requests to distinct port ordinals. A load issues its address and chip-enable,
waits for the configured synchronous read latency, and then makes its registered
result available to dependent operations. A store drives address, write-enable,
and data in its scheduled state.

`HI_MemoryObjectAnalysis` owns memory discovery, address provenance, access
binding, and depth resolution for arguments and local allocations. Address
provenance is traced through GEP,
`ptrtoint`/`inttoptr`, integer add/subtract, casts, selects, and integer PHI
cycles. An access that cannot be traced to exactly one top-level pointer is
rejected. Light-HLS's final LSR IR uses element-address increments after
`ptrtoint`; the RTL backend preserves that convention so `address + 1` selects
the next memory element.

Memory depth is resolved independently for each object. A local array uses its
exact static allocation size. An external pointer uses the following priority
order:

1. A source array extent recovered by the Clang preprocessor. Multidimensional
   extents are flattened to their scalar element count.
2. An explicit `memory_depth` entry in the Light-HLS configuration.
3. A conservative minimum depth inferred from statically bounded SCEV/GEP
   address ranges in the transformed LLVM IR.
4. Unknown depth when none of the preceding sources is available.

Source and configured depths are exact declarations. An IR-inferred value is
only a lower bound because the observed accesses do not prove the physical
allocation size. Reports therefore separate `depth`, `depth_source`,
`depth_exact`, and `inferred_minimum_depth`. A declared depth smaller than the
inferred minimum is rejected with `BACKEND_MEMORY_DEPTH_TOO_SMALL`. If a source
extent exists, it has priority over a duplicate configuration and the ignored
configuration produces a warning.

The emitted address width is `max(1, ceil(log2(depth)))`. Unknown depths retain
the conservative 64-bit element-address interface. Depth affects interface
width, local storage sizing, and later BRAM inference. Port count and read
latency affect scheduling and RTL timing directly.

`internal_ram` describes the backend storage abstraction, not a forced FPGA
primitive. Vivado may map a shallow array to registers or distributed RAM and a
larger eligible array to BRAM. Physical BRAM usage is therefore taken only from
`vivado_report/vivado_report.json`, never inferred from the object label alone.

The configuration syntax is:

```text
memory_depth variable=<top-parameter> scope=<top-function> depth=<positive-integer>
memory_interface variable=<memory-name> scope=<top-function> ports=<1-or-2> read_latency=<positive-integer>
```

For example:

```text
memory_depth variable=memory scope=backend_memory_depth depth=16
memory_interface variable=memory scope=backend_dual_memory ports=2 read_latency=1
```

For a local array, `variable` is the LLVM allocation/source variable name. The
supported read-latency range is 1 through 64 cycles. If no interface entry is
present, the default is one port and one-cycle read latency.

The current binder groups operations by LLVM opcode and result bit width. It
reuses an instance across mutually exclusive basic blocks and non-overlapping
cycles, and allocates enough instances for the maximum same-cycle concurrency.

`HI_StorageBindingAnalysis` runs after array partitioning so a directive is
applied consistently to every physical bank. Source syntax follows Vitis HLS:

```cpp
#pragma HLS BIND_STORAGE variable=buffer type=ram_2p impl=uram latency=2
```

The supported implementation names are `auto`, `bram`, `uram`, and `lutram`.
The generated local memory receives the corresponding Vivado `ram_style`
attribute. The selected port count and latency also drive scheduling; the
attribute is not merely report metadata. External `ap_memory` arguments are
caller-owned and are therefore not valid `BIND_STORAGE` targets.

`HI_DataflowAnalysis` discovers direct calls in the top region and constructs a
task/channel graph before instruction scheduling. `hls::stream` allocations
use their explicit STREAM width, depth, instance count, and optional initial
token count. A stream array is lowered to one FIFO per flattened stream object.
One producer feeding several consumers is a broadcast: analysis creates one
edge and one physical FIFO per consumer so every consumer observes every
element. Multiple producers remain illegal because the current backend has no
defined arbitration or ordering rule.

The ordinary zero-initial-token FIFO elaborates a reset-free payload array with
an automatic RAM inference attribute; reset affects only pointers and occupancy.
This permits distributed RAM or an equivalent small-memory implementation
instead of one resettable register per payload bit. A separately elaborated
initialized-token branch clears its payload on reset so initialized cycles keep
their defined zero-token semantics.

A local array shared between two tasks is represented as a PIPO. The generated
channel has two transaction buffers. An unpartitioned array has one memory per
buffer; a cyclic, block, or complete partition has one memory or scalar bank per
logical partition bank in each buffer. Ordinary array payload banks have no
reset and expose only synchronous RAM reads, allowing Vivado to infer LUTRAM or
BRAM. Complete-partition scalar banks retain a separately elaborated
combinational peek path. All physical banks of one partitioned channel share a
single pair of ready bits and one read/write-bank ownership state. The producer
owns one write buffer until task completion commits it, while the consumer owns
the other ready buffer until task completion releases it. Buffer roles alternate
across transactions. A transfer may cover only part of the declared array when
the exact producer and consumer rates are equal and do not exceed its capacity;
unused elements do not participate in that transaction.

`HI_DataflowTaskGeneration` derives each task's transaction latency from the
generated RTL FSM state costs and static loop trip counts. The reported task
reacceptance interval includes the one edge needed by the generated
`ap_ctrl_hs` task to restart after completion. Conditional control flow is
reported as a conservative structural bound instead of an exact value.
`HI_DataflowPerformanceAnalysis` counts statically bounded reads and writes per
task transaction using LoopInfo and ScalarEvolution. It requires balanced
producer/consumer rates, records exact or bounded rates and their runtime
contract, checks FIFO/PIPO capacity and minimum buffering, distinguishes a
cycle from a zero-token startup cycle, and estimates steady-state graph II as
the maximum generated task reacceptance interval. For an acyclic all-PIPO graph,
the first-transaction latency is the exact PIPO-constrained critical path plus
one-cycle handoffs and terminal retirement. A FIFO graph with exact static
rates, one physical FIFO per channel, and regular single-loop access schedules
is evaluated by bounded token simulation. The simulator advances each generated
task's local FSM cycle, freezes the whole task on empty/full stalls, applies
concurrent FIFO reads and writes using pre-edge occupancy, and includes terminal
retirement. Each channel reports the producer and consumer first-access cycle,
recurrence interval, and timing-exactness flag used by this model. Graphs
outside that exact subset retain an explicit structural lower/upper range. A
graph with public `ap_fifo` backpressure reports only the
structural lower bound because the environment can stall it indefinitely. An
initialized edge is removed from the
startup-dependence graph; consequently a directed cycle is legal only when its
initial tokens break every zero-token cycle. Initial token payloads are zero in
the current STREAM extension. These values are emitted in
`dataflow_graph.rpt` and `dataflow_graph.json` before RTL generation. Unknown or
conditional PIPO rates are rejected. A bounded or conditional FIFO is accepted
only with a runtime rate contract: backpressure protects individual transfers,
but the caller must select paths whose producer and consumer counts balance.
Its performance result is marked invalid when a static graph II cannot be
justified.

Each task is compiled through the regular Light-HLS backend in an independent
context. PIPELINE and ARRAY_PARTITION directives are filtered by task scope so
one task never receives another task's directives. `HI_DataflowRTLGeneration`
instantiates the resulting submodules, connects bounded FIFOs or banked PIPOs,
and propagates channel availability by clock-enabling or pausing the affected
task controller. FIFO read data is registered to match the one-cycle
`ap_memory` read contract. PIPO consumers cannot start until one transaction
buffer is committed.

The DATAFLOW top controller is persistent rather than a one-shot wave barrier.
It keeps a compact pending count for each task; only PIPO consumers need a
two-bit count at the current outstanding capacity. Transaction-scalar queues
and per-task scalar read-bank bits are omitted when the top has no scalar
arguments. A single-terminal graph retires directly from terminal `ap_done`;
per-terminal completion credits are allocated only for joins with multiple
terminal tasks. Task starts can consume a top-level acceptance directly, PIPO
consumer starts look ahead to producer commit, and producer starts look ahead
to consumer release. A completed task may therefore restart without the former
done-low bubble while independent tasks from later top transactions are active.
The report currently declares two maximum outstanding top transactions and a
minimum burst acceptance interval of one cycle. `ap_done` is issued in
accepted-transaction order when all terminal tasks for that transaction have
completed.

Top-level scalar stream references use Vitis-style `ap_fifo` signals. Inputs
emit `<name>_dout`, `<name>_empty_n`, and `<name>_read`; outputs emit
`<name>_din`, `<name>_full_n`, and `<name>_write`. External empty/full
backpressure participates in the same task clock-enable contract as internal
channels. Because the current source frontend does not recover a top stream's
element width reliably, the supported public-stream form requires a STREAM
directive with `bitwidth=<N>`. This is a Light-HLS frontend extension and must
be normalized explicitly in any Vitis comparison harness.

## Explicitly unsupported features

The initial implementation intentionally rejects:

- dynamic-trip, multi-block, branched, or otherwise noncanonical loop
  pipelines;
- multiple PIPELINE directives on one nested-loop chain;
- nested DATAFLOW regions, conditional or indirect task calls, multi-producer
  fan-in, conditional or unknown-rate PIPO transfers, dynamically sized PIPO
  transfers, top-level stream arrays, and public stream ports outside a
  DATAFLOW top;
- floating-point operations and arbitrary-width `ap_int`/`ap_uint`/`_BitInt`
  values whose LLVM ABI representation is not a native integer supported by
  the legacy Light-HLS evaluator;
- global memories, dynamically sized local arrays, volatile or atomic accesses,
  mixed-width accesses to one memory, and more than two memory ports;
- calls that remain after inlining and LLVM instructions outside the documented
  integer subset.

Unsupported features are never silently discarded. The backend exits with a
nonzero status and emits `rtl/failure.json` with stable diagnostic codes. This
behavior is important for RL training because an invalid action receives an
unambiguous failure instead of a misleading QoR value.

Loop unrolling is supported when the earlier Light-HLS unroll pass has already
materialized the transformed integer/control-flow IR. The RTL backend consumes
that IR and does not need a separate unroll implementation.

## Reserved extension interfaces

`HI_BackendCommon/HI_BackendContext.h` provides typed interfaces for current and
later backend work:

- `LoopPipelineInfo` records the loop structure, requested/achieved II, lower
  bounds, stages, per-operation schedule, and iteration offsets;
- `MemoryObject` and `MemoryAccessBinding` drive external `ap_memory` and local
  RAM lowering. `MemoryDepthSource` preserves whether each depth came from the
  frontend, an allocation, configuration, or conservative inference.
  `MemoryAccessMode` and `MemoryPortUsage` preserve whole-object and per-port
  direction. `PartitionKind`, `MemoryPartitionDimension`, per-bank port usage,
  exact possible-bank sets, and explicit storage-binding type and
  implementation describe implemented array banking and RAM selection;
- `DataflowTask`, `DataflowChannel`, and `DataflowGraph` represent task-level
  scheduling, persistent execution limits, exact or bounded rates, initial
  tokens, broadcast edges, PIPO partition layout, and zero-token-cycle state;
- `OperatorImplementation` separates LLVM operations from integer, floating
  point, or vendor IP implementations;
- `RTLInterfaceProtocol` reserves scalar, `ap_ctrl_hs`, `ap_memory`, AXI, FIFO,
  and BRAM protocols.

These interfaces allow later features to extend scheduling and RTL lowering
without changing the pass boundary or report schema.

## Output layout and QoR semantics

Each run writes to `<output-root>/reports`. Without `--output`, the output root
is the directory from which Light-HLS was launched. With `--output <directory>`,
that directory becomes the output root:

```text
<output-root>/reports/
├── report.rpt
├── manifest.json
├── .lighthls/               # pass logs, rewritten source, and LLVM IR
├── estimate/
│   ├── pre_backend_estimate.json
│   └── pre_backend_estimate.rpt
├── scheduling/
│   ├── schedule.json
│   ├── schedule.rpt
│   ├── pipeline_schedule.json
│   ├── pipeline_schedule.rpt
│   ├── dataflow_graph.json
│   ├── dataflow_graph.rpt
│   └── post_schedule_qor.json
├── binding/
│   ├── binding.json
│   ├── binding.rpt
│   └── post_binding_qor.json
├── rtl/
│   ├── top.v                 # successful run
│   ├── failure.json          # rejected run
│   └── rtl_generation.rpt
├── vivado_report/
│   ├── vivado_report.json
│   ├── vivado_report.rpt
│   ├── utilization.rpt       # requested synthesis only
│   ├── timing_summary.rpt    # requested synthesis only
│   ├── post_synth.dcp        # requested synthesis only
│   ├── vivado.log            # requested synthesis only
│   ├── run_vivado.tcl        # requested synthesis only
│   └── vivado_metrics.txt    # requested synthesis only
└── summary/
    ├── qor_comparison.json
    └── qor_summary.rpt
```

JSON remains the stable machine-readable interface for regression scripts and
RL training. The `.rpt` files provide Vitis-style titled sections and tables
for human inspection. Start with `report.rpt`, then open the report for the
stage of interest. Intermediate files are always emitted; the `DEBUG` argument
is no longer required.

`binding.json` records `access_mode` for every memory object and a `port_usage`
entry for every configured port. Partitioned memories additionally report
source dimensions, the kind/factor/local extent of every partitioned dimension,
bank count, bank depth/address width, bank interface, per-bank port usage, and
each access's static or dynamic possible-bank set. `binding.rpt` exposes the
same information in its `Array Partition Layout` and `Memory Access Bindings`
tables. Pipeline reports include bank sets and whether the mapping proof is
exact.
Storage-bound memories additionally report `binding_type`, `implementation`,
and whether the choice came from an explicit pragma. The DATAFLOW graph report
records the persistent execution model and limits, task functions and local
pipeline summaries, generated-FSM task latency and its provenance, graph II,
first-transaction latency bounds, PIPO handoff and retirement overhead, graph
topology and startup-token state, and every channel's
producer, consumer, type, element width, FIFO depth, flattened instance count,
exact or bounded rates, runtime-rate contract, initial-token count, broadcast
state, and PIPO partition/bank layout.

The stages have deliberately different meanings:

- `pre_backend_estimate` is the heuristic evaluator result;
- `post_scheduling` reports scheduled control states and a static latency for
  acyclic control flow and supported fixed-trip pipeline loops. Other cyclic
  control flow reports `latency_cycles: null`;
- `post_binding` reports structural operator, register-bit, and mux counts. It
  must not be interpreted as synthesized LUT or FF usage;
- `post_rtl_synthesis` is a measured Vivado result when `--synthesis enable` is
  selected. It reports the tool/part, runtime, LUT, FF, DSP, BRAM tiles, worst
  slack, and critical-path delay. With synthesis disabled it records
  `not_requested` rather than presenting an estimate as measured QoR.

`--freq 100MHz` sets a 100 MHz target and therefore a 10 ns clock period. The
option is mandatory for every Light-HLS invocation. It accepts `Hz`, `kHz`,
`MHz`, or `GHz` and overrides the clock in the config file or
`HI_CLOCK_PERIOD`; neither legacy source supplies a fallback when `--freq` is
absent. Both the target frequency and derived period are recorded in every JSON
and human-readable report. The old misspelling `--fre` is rejected with a
correction diagnostic. Vivado synthesis is disabled by default and runs only
when `--synthesis enable` is explicitly selected.

Every JSON file includes the schema version, stage, validity, design, top
function, clock, board, pragma configuration, explicit memory-depth
configuration, memory-interface configuration, pass sequence, source hash,
warnings, and errors.

## Target-board policy

U250 (`xcu250-figd2104-2L-e`) is the primary implementation and comparison
target. Artix-7 A12T (`xa7a12tcsg325-1q`) is retained as a secondary portability
reference. Search rewards and design comparisons must be computed within one
target board: raw LUT/FF/timing values from different FPGA families must not be
combined as if they were directly comparable.

The independent `HI_RTLSynthesis` pass invokes Vivado only after successful RTL
emission. The board part is read from `HLS_Data_Lib/boards/<board>/board.json`,
or can be overridden with `--synthesis-part`. A missing tool, invalid part,
failed synthesis, or incomplete report produces a stable backend diagnostic.
Generated kernels are synthesized out of context with rebuilt hierarchy. This
matches the common Light-HLS/Vitis kernel-level comparison and avoids treating
the HLS interface as a package-pin top level. On Linux, Vivado is limited to one
worker and pinned to the first CPU available to the Light-HLS process. This is a
stability workaround for reproducible Vivado 2025.2 U250 crashes on the local
Intel hybrid-core host. The selected mode, thread limit, and CPU affinity are
recorded in `vivado_report.rpt` and `vivado_report.json`. Systems without the
Linux `taskset` utility retain the single-worker setting without CPU pinning.

Run both boards, with U250 first and marked primary in the aggregate report:

```bash
Light_HLS/Tests/RTL_Backend/run_board_synthesis_comparison.sh \
  Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top \
  /tmp/light-hls-board-comparison
```

For a fair Light-HLS/Vitis HLS comparison on the primary board, use the common
U250 flow instead. It generates both RTL implementations from one source, checks
their post-synthesis port signatures, optionally runs one Icarus testbench
against both, and synthesizes both with the exact same Vivado OOC Tcl flow:

```bash
Light_HLS/Tests/FairComparison/run_backend_scalar_u250.sh \
  Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top \
  /tmp/backend-scalar-u250-fair
```

The resulting `u250_fair_comparison.json` is marked `reward_eligible` only when
the interface signatures and functional `RESULT` traces match. Vitis
`csynth.xml` values remain separate estimates; only common-flow Vivado metrics
are used for measured resource/timing comparison. See
`Light_HLS/Tests/FairComparison/README.md` for the full comparison contract and
verified baseline.

## Build and test

Build the learner libraries and top executable:

```bash
cd Light_HLS
./BuildLibs.sh
Tests/Light_HLS_Top/Build.sh -DHI_BUILD_LEARNER_LIBS=OFF
```

Run the scalar backend case:

```bash
mkdir -p /tmp/light-hls-backend-scalar
cd /tmp/light-hls-backend-scalar
/path/to/Light_HLS_Top \
  /path/to/Light_HLS/App/backend_scalar/backend_scalar.c \
  backend_scalar --freq 100MHz --synthesis disable \
  --output /tmp/backend_scalar_output
```

Compile and run the generated RTL testbench:

```bash
iverilog -g2012 -Wall -s backend_scalar_tb \
  -o /tmp/backend_scalar_sim \
  /tmp/backend_scalar_output/reports/rtl/top.v \
  /path/to/Light_HLS/App/backend_scalar/backend_scalar_tb.v
vvp /tmp/backend_scalar_sim
```

Run the two-memory `ap_memory` case:

```bash
mkdir -p /tmp/light-hls-backend-memory
cd /tmp/light-hls-backend-memory
/path/to/Light_HLS_Top \
  /path/to/Light_HLS/App/extremeLoop/extremeLoop.cpp \
  extremeLoop --freq 100MHz --synthesis disable \
  --output /tmp/backend_memory_output
iverilog -g2012 -Wall -s extremeLoop_tb \
  -o /tmp/backend_memory_sim \
  /tmp/backend_memory_output/reports/rtl/top.v \
  /path/to/Light_HLS/App/extremeLoop/extremeLoop_tb.v
vvp /tmp/backend_memory_sim
```

Run a pointer-memory case with an exact configured depth:

```bash
mkdir -p /tmp/light-hls-backend-depth
cd /tmp/light-hls-backend-depth
/path/to/Light_HLS_Top \
  /path/to/Light_HLS/App/backend_memory_depth/backend_memory_depth.cpp \
  backend_memory_depth \
  /path/to/Light_HLS/App/backend_memory_depth/backend_memory_depth.cfg \
  --freq 100MHz --synthesis disable --output /tmp/backend_depth_output
iverilog -g2012 -Wall -s backend_memory_depth_tb \
  -o /tmp/backend_depth_sim \
  /tmp/backend_depth_output/reports/rtl/top.v \
  /path/to/Light_HLS/App/backend_memory_depth/backend_memory_depth_tb.v
vvp /tmp/backend_depth_sim
```

The complete regression also checks source/final-IR/RTL scalar equivalence,
source/inferred/configured memory depths, undersized-depth rejection, local RAM
with two-cycle reads, single-port conflict serialization, and same-cycle
dual-port binding:

```bash
Light_HLS/Tests/RTL_Backend/run_iverilog_test.sh \
  Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top
```

Run the array-partition regression separately:

```bash
Light_HLS/Tests/ArrayPartitionBackend/run_array_partition_tests.sh \
  Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top
```

The array-partition suite includes cyclic, block, complete, multidimensional,
local, and complete-partition pipeline cases. The common U250 comparison batch
is available as `Tests/FairComparison/run_array_partition_u250_comparisons.py`.

The suite covers cyclic, block, read-only and read/write complete,
multidimensional `dim=0`, partial
multidimensional cyclic, local RAM, invalid-dimension rejection, bank-aware II,
report metadata, and RTL behavior.

Run the DATAFLOW regression separately:

```bash
Light_HLS/Tests/DataflowBackend/run_dataflow_backend_tests.sh \
  Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top
```

It covers FIFO depth and backpressure, repeated and overlapped top
transactions, local task pipelines, external and internal cyclic/complete
partitioning, combined PIPELINE plus ARRAY_PARTITION, full and partial PIPOs,
broadcast fanout, initialized cycles, public `ap_fifo` stalls, graph-report
contracts, stable invalid-graph diagnostics, LLVM verification, and
self-checking Icarus simulation. Representative combined and public-FIFO cases
are also accepted by Vivado OOC synthesis for U250 at 100 MHz.

Existing pragma-free applications can be checked separately with deterministic
source-to-RTL memory comparisons:

```bash
Light_HLS/Tests/AppCorrectness/run_supported_apps.sh \
  Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top
```

This application regression currently covers `2dloop1darray_notpl` and
`spam-filter`. See `Light_HLS/Tests/AppCorrectness/README.md` for the supported
feature boundary and the reason backend-oriented memory bitcode is verified but
not used as a native execution oracle.
