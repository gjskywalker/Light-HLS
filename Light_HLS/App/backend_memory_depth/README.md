# Memory depth source tests

The pointer parameter deliberately has no source-level array extent. Running
without a config exercises conservative IR range inference and produces a
minimum depth of four. Running with `backend_memory_depth.cfg` exercises the
explicit `memory_depth` configuration and produces an exact depth of sixteen.

`backend_memory_depth_too_small.cfg` declares a depth of two. The backend rejects
it because the transformed IR proves accesses through element index three.

The self-checking Icarus testbench targets the configured-depth RTL.
