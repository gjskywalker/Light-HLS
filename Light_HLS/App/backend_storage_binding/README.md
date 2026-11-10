# Explicit Storage Binding

This App exercises local-memory `BIND_STORAGE` directives for block RAM,
UltraRAM, and distributed RAM. The backend maps `ram_1p` and `ram_2p` to one
or two logical ports, applies the configured read latency to scheduling, and
emits a Vivado `ram_style` attribute on every generated bank.

Top-level pointer arguments remain direction-aware `ap_memory` interfaces.
Their physical storage is owned by the caller and is therefore configured
with `INTERFACE storage_type`, not local `BIND_STORAGE`.
