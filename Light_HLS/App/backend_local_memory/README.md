# Local RAM backend test

This design keeps an eight-element local integer array in the optimized LLVM
IR. The backend lowers it to an internal synchronous RAM instead of exposing a
top-level `ap_memory` interface.

The configuration selects two ports and a two-cycle modeled read latency. The
initialization loop and two final reads exercise local write/read lowering and
the configurable read-data pipeline. The separate `backend_dual_memory` case
isolates same-cycle dual-port binding.

The array is intentionally shallow. On U250, Vivado may implement it as
distributed RAM rather than a BRAM tile; the test validates RAM semantics and
synthesizability, not a forced physical primitive.
