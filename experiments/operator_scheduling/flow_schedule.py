"""Scheduling model and worker for the Light-HLS workflow pilot."""

from collections import Counter, deque
from itertools import permutations
import json
import math
from pathlib import Path


def check(graph, starts):
    nodes = graph["nodes"]
    if not isinstance(starts, list) or len(starts) != len(nodes):
        raise ValueError("expected one start cycle per operation")
    if any(type(s) is not int or not 0 <= s <= 100000 for s in starts):
        raise ValueError("expected start cycles in [0, 100000]")
    ends = [start + node["latency"] for start, node in zip(starts, nodes)]
    for source, sink in graph["edges"]:
        if ends[source] > starts[sink]:
            raise ValueError(f"operation {sink} starts before its dependency {source} finishes")
    usage = Counter()
    for start, node in zip(starts, nodes):
        for resource in node["resources"]:
            usage[resource, start] += 1
            if usage[resource, start] > graph["ports"][resource]:
                raise ValueError(f"port capacity exceeded for {resource} at cycle {start}")
    returned = graph["return_node"]
    return max(1, max(ends, default=0), ends[returned] + 1 if returned >= 0 else 0)


def port_delays(graph, starts):
    """Describe delayed requests and name the occupants of their ready cycle."""
    ready = [0] * len(starts)
    for source, sink in graph["edges"]:
        ready[sink] = max(ready[sink], starts[source] + graph["nodes"][source]["latency"])
    return [
        {"node": i, "ready": ready[i], "issued": starts[i], "ports": node["resources"],
         "other_requests_at_ready": [j for j, other in enumerate(graph["nodes"])
                                     if j != i and starts[j] == ready[i]
                                     and set(node["resources"]).intersection(other["resources"])]}
        for i, node in enumerate(graph["nodes"]) if node["resources"] and starts[i] > ready[i]
    ]


def list_schedule(graph, priority, *, trace=None):
    nodes = graph["nodes"]
    parents = [set() for _ in nodes]
    children = [set() for _ in nodes]
    for source, sink in graph["edges"]:
        parents[sink].add(source)
        children[source].add(sink)
    tail = [0] * len(nodes)
    downstream_requests = [0] * len(nodes)
    for i in reversed(range(len(nodes))):
        tail[i] = nodes[i]["latency"] + max(
            int(i == graph["return_node"]), max((tail[j] for j in children[i]), default=0)
        )
        downstream_requests[i] = max(
            (bool(nodes[j]["resources"]) + downstream_requests[j] for j in children[i]), default=0
        )
    starts = [None] * len(nodes)
    pending = set(range(len(nodes)))
    cycle = 0
    used = Counter()
    while pending:
        ready = [i for i in sorted(pending) if all(
            starts[j] is not None and starts[j] + nodes[j]["latency"] <= cycle for j in parents[i]
        )]
        proposals = []
        for i in ready:
            resources = nodes[i]["resources"]
            if any(used[r] >= graph["ports"][r] for r in resources):
                continue
            score = priority({
                "opcode": nodes[i]["opcode"], "latency": nodes[i]["latency"], "tail": tail[i],
                "fanout": len(children[i]), "downstream_requests": downstream_requests[i],
                "uses_port": bool(resources), "cycle": cycle,
                "competing_requests": sum(bool(set(resources).intersection(nodes[r]["resources"]))
                                          for r in ready if r != i),
            })
            if type(score) not in (int, float) or not math.isfinite(score):
                raise ValueError("priority() must return a finite number")
            proposals.append((score, -i))
        if proposals:
            _, negative = max(proposals)
            chosen = -negative
            if trace is not None:
                trace.append({"cycle": cycle, "ready": ready, "eligible": [-j for _, j in proposals],
                              "chosen": chosen})
            starts[chosen] = cycle
            used.update(nodes[chosen]["resources"])
            pending.remove(chosen)
        else:
            cycle += 1
            used.clear()
    return starts


def exact_single_port(graph):
    """Enumerate request orders and take earliest starts for each acyclic order."""
    nodes = graph["nodes"]
    requests = [i for i, node in enumerate(nodes) if node["resources"]]
    if list(graph["ports"].values()) != [1] or len(requests) > 8:
        raise ValueError("reference supports one single port with at most eight requests")
    best = None
    for order in permutations(requests):
        outgoing = [[] for _ in nodes]
        degree = [0] * len(nodes)
        edges = [(i, j, nodes[i]["latency"]) for i, j in graph["edges"]]
        edges.extend((i, j, 1) for i, j in zip(order, order[1:]))
        for i, j, lag in edges:
            outgoing[i].append((j, lag))
            degree[j] += 1
        ready = deque(i for i, d in enumerate(degree) if not d)
        starts = [0] * len(nodes)
        while ready:
            i = ready.popleft()
            for j, lag in outgoing[i]:
                starts[j] = max(starts[j], starts[i] + lag)
                degree[j] -= 1
                if not degree[j]:
                    ready.append(j)
        if not any(degree):
            candidate = check(graph, starts), starts
            if best is None or candidate < best:
                best = candidate
    if best is None:
        raise ValueError("no feasible request order")
    return best


if __name__ == "__main__":
    import resource
    import runpy
    import sys

    resource.setrlimit(resource.RLIMIT_AS, (512 << 20, 512 << 20))
    resource.setrlimit(resource.RLIMIT_FSIZE, (1 << 20, 1 << 20))
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    data = json.loads(Path("/graph.json").read_text())
    rule = runpy.run_path("/policy.py")["priority"]
    trace = [] if "--trace" in sys.argv else None
    Path("/output/schedule.json").write_text(json.dumps(list_schedule(data, rule, trace=trace)))
    if trace is not None:
        Path("/output/trace.json").write_text(json.dumps(trace))
