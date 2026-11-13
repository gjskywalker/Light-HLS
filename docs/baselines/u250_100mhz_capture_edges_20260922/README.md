# U250 FIFO capture and transaction-edge evidence

See the [checkpoint notes](../../logs/FIFO_CAPTURE_AND_TRANSACTION_EDGES_20260922.md)
for implementation contracts, results, and limitations.

- `before.json/csv`: latest preceding measurement for each of eight cases.
- `after.json/csv/rpt`: eight measurements from this optimization session.
- `qor_delta.json`: same-contract gate, six improved and two unchanged cases;
  no cycle/resource regression, all synthesis timing checks passing.
- `experiments.json`: measured FIFO-capture-only, rejected automatic-dual-port,
  and recurrence-sharing intermediate candidates. These are not release results.
- `release_rtl_coverage.json`: byte-identical measured RTL coverage for 21
  distinct sources (13 historical, seven entry candidate, one release scalar).
  Original compiler fingerprints are retained; no measurement is rewritten.
- `local_regressions.json/rpt`: all 17 unified suites pass.
- `app_smoke.json`: all 18 App smoke checks pass.
- `app_capability_matrix.json`: 24 PASS, 12 EXPECTED_REJECTION, one NOT_APPLICABLE.
- `fifo_events.json`, `ordinary_backedge.json`, `ordinary_loop_edges.json`, and
  `transaction_edges.json`: directed suite results and source/binary hashes.

All archived files above are byte-for-byte copies of generated reports. The
combined RTL coverage report selects matching entries from the three raw
identity audits named inside it; it excludes the superseded scalar candidate.

Raw root: `/tmp/lighthls-continuous-qor-20260922.g3KrQD/`.
Release executable SHA256:
`d0b0d06f0be9d11446e5257f9cad9b5ea4412902faa96bbf5052e4d6b00eea25`.
Entry candidate measurement SHA256:
`573bb5db3a93fdbc88471d6e98d798235112cc8c9e4715c213364cf81853b6e8`.

The baseline, benchmark, configuration, testbench, synthesis flow, tool version,
and FPGA/clock contracts are checked by `compare_u250_baselines.py`. These local
simulation and identity checks do not establish formal equivalence for all
inputs or guarantee timing after placement and routing.
