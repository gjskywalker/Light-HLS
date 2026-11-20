"""Deterministic critical-path-priority list scheduler for HeuriGym DAGs."""

from collections import Counter, defaultdict, deque
import heapq
import json
from pathlib import Path


def solve(input_file: str, solution_file: str) -> None:
    graph = json.loads(Path(input_file).read_text())
    kinds = dict(graph["nodes"])
    delays = {node: graph["delay"][kind] for node, kind in kinds.items()}
    successors = defaultdict(list)
    indegree = dict.fromkeys(kinds, 0)
    for source, target, _ in graph["edges"]:
        successors[source].append(target)
        indegree[target] += 1

    remaining = indegree.copy()
    queue = deque(node for node in kinds if not remaining[node])
    order = []
    while queue:
        node = queue.popleft()
        order.append(node)
        for target in successors[node]:
            remaining[target] -= 1
            if not remaining[target]:
                queue.append(target)
    tail = {}
    for node in reversed(order):
        tail[node] = delays[node] + max((tail[target] for target in successors[node]), default=0)

    ready = [(-tail[node], node) for node in kinds if not indegree[node]]
    heapq.heapify(ready)
    busy = []
    occupied = Counter()
    starts = {}
    cycle = 0
    while ready or busy:
        while busy and busy[0][0] == cycle:
            _, node = heapq.heappop(busy)
            occupied[kinds[node]] -= 1
            for target in successors[node]:
                indegree[target] -= 1
                if not indegree[target]:
                    heapq.heappush(ready, (-tail[target], target))
        blocked = []
        while ready:
            priority, node = heapq.heappop(ready)
            kind = kinds[node]
            if occupied[kind] >= graph["resource"][kind]:
                blocked.append((priority, node))
                continue
            starts[node] = cycle
            occupied[kind] += 1
            heapq.heappush(busy, (cycle + delays[node], node))
        for entry in blocked:
            heapq.heappush(ready, entry)
        if busy:
            cycle = busy[0][0]

    Path(solution_file).write_text("".join(f"{node}:{starts[node]}\n" for node in sorted(starts)))
