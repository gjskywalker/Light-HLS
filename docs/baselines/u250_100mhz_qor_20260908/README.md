# Clock-aware backend optimization measurements

This archive contains 20 unique U250 comparisons: the 19 Phase-1 designs and
one additional nested PIPELINE + partitioned DATAFLOW design. Measurements
use Vitis/Vivado 2025.2, `xcu250-figd2104-2L-e`, a 10 ns constraint, common
Icarus result traces, normalized interfaces, and the same Vivado OOC flow.

- `u250_baselines.rpt`: human-readable cycles, LUT, FF, and timing table.
- `u250_baselines.csv`: compact machine-readable comparison rows.
- `u250_baselines.json`: complete compact measurements and input fingerprints.
- `local_regressions.json`: all eight final local suites pass.
- `rtl_identity_audit.json` and `nested_rtl_identity_audit.json`: all 20 final
  RTL inputs match their measurements, modulo independent zero-assignment order.

All measurements pass functional and normalized-interface checks. Measured
Vivado resources are distinct from HLS estimates or structural binding counts.
The purely combinational Vitis complete-partition case has no registered timing
path; its missing WNS must not be interpreted as a timing failure or a measured
maximum frequency.

The final compiler is validated against these generated RTL inputs separately.
Original measured executable fingerprints are retained, including the dedicated
nested-DATAFLOW check and the FIFO/PIPO reruns after deterministic channel ordering.
Do not treat a retained measurement as a fresh run of a different compiler hash.
Two independent regenerations of the 19 original cases using the final compiler
also produce byte-identical RTL, without normalization.

Final executable SHA256:
`243e9e01583fb510c81e80ddc9987d4c3a7d82b1f2347980bf7e967e0eef643a`.

Raw reports, the final eight-suite regression, and identity-audit JSON files
remain under `/tmp/lighthls-qor-20260908.71Q3iI/`. Failed intermediate experiments
are kept separately and are not included in this archive. The immutable
Phase-1 archive in `../u250_100mhz/` is not modified.

See [the optimization notes](../../QOR_OPTIMIZATION_20260908.md) for scope,
comparison conventions, measured improvements, and remaining performance gaps.
