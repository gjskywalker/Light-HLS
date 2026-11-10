# Local write-data binding tests

The binder identifies a local memory bank/port with exactly one static store
producer. Its data input can be a continuous wire: CE/WE, address selection,
pipeline valid tokens, and task clock enable still determine whether a write
actually occurs. Multiple writers and external interfaces retain the existing
payload mux. No inactive-cycle external-port contract is relaxed.

```bash
bash Light_HLS/Tests/LocalWriteData/run_tests.sh \
  Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top
```

Use `--output /absolute/fresh/directory` to name the artifact directory.
Otherwise a unique `/tmp/light-hls-local-write.*` directory is retained.
The suite is registered in the unified regression driver.

| Fixture | Coverage | Direct local write ports |
| --- | --- | ---: |
| `single_ordinary` | Ordinary cyclic-banked local memory | 4 |
| `single_pipeline` | Pipeline-produced payload and dynamic bank selection | 4 |
| `single_latency2` | Pipeline producer with two-cycle local read latency | 4 |
| `multiple_ordinary` | Two static writers share each bank/port; retain muxes | 0 |
| `multiple_dual` | Two simultaneous writers bind to separate ports | 2 |
| `partial_banks` | Bank zero has two writers; other banks have one | 3 |
| `external_only` | External write data retains its original control | 0 |
| `local_dataflow` | Private RAM inside a pipelined PIPO task, with injected task stalls | 1 |

Each fixture runs 64 calls without reset, checking 1,024 outputs. Across the
suite, this is 512 calls and 8,192 output checks. Inputs use deterministic
xorshift data; scalar ports change immediately after start acceptance. Read
counts, duplicate/lost writes, and captured arguments are checked.

For the DATAFLOW fixture, the testbench temporarily forces the wrapper's shared
task clock-enable low. This also gates external requests and channel events.
Every call must exercise a paused local write, commit exactly 32 local writes,
and leave all local RAM words unchanged on paused edges. This is controlled
stall injection, not a claim that the fixture naturally fills a FIFO.

Standalone binding JSON is checked against the actual writer count for every
bank/port. Generated RTL must use continuous assignments for exactly the
expected ports and must not also drive them procedurally. The current DATAFLOW
wrapper does not export child binding reports, so its child optimization is
checked in generated RTL and simulation rather than parent binding counts.

## Historical stream failure (now covered by FIFOEvents)

`known_failures/private_stream_buffer.cpp` retains its historical path and
source contents for reproducibility. The September 21 FIFO event-preservation
change makes it a required positive test in the separate
[FIFOEvents suite](../FIFOEvents/README.md), including natural depth-one FIFO
backpressure and private RAM freeze checks. The eight tests above retain their
original PIPO coverage. Historical failure evidence remains in the September
18 checkpoint; it does not describe the current expected behavior.
