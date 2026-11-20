"""Invoke a single solver inside the evaluator's isolated process."""

import resource
import runpy


resource.setrlimit(resource.RLIMIT_AS, (1 << 30, 1 << 30))
resource.setrlimit(resource.RLIMIT_FSIZE, (2 << 20, 2 << 20))
resource.setrlimit(resource.RLIMIT_CPU, (10, 11))
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
solver = runpy.run_path("/solver.py")
solver["solve"]("/graph.json", "/output/schedule.txt")
