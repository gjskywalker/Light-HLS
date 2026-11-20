import json
import heapq
import random
import time
from bisect import bisect_right, insort
from collections import deque


def solve(input_file: str, solution_file: str):
    began = time.perf_counter()
    deadline = began + 8.4
    with open(input_file, 'r', encoding='utf-8') as f:
        data = json.load(f)

    nodes = data['nodes']
    n = len(nodes)

    def write_solution(starts):
        with open(solution_file, 'w', encoding='utf-8') as f:
            f.writelines(str(nodes[i][0]) + ':' + str(starts[i]) + '\n'
                         for i in range(n))

    if not n:
        write_solution([])
        return

    index = {node[0]: i for i, node in enumerate(nodes)}
    type_index = {}
    kinds = []
    for _, typ in nodes:
        if typ not in type_index:
            type_index[typ] = len(type_index)
        kinds.append(type_index[typ])
    nt = len(type_index)
    durations = [0] * nt
    capacities = [0] * nt
    counts = [0] * nt
    for k in kinds:
        counts[k] += 1
    for typ, k in type_index.items():
        durations[k] = int(data['delay'][typ])
        capacities[k] = min(int(data['resource'][typ]), counts[k])
    duration = [durations[k] for k in kinds]

    succ = [[] for _ in range(n)]
    pred = [[] for _ in range(n)]
    for edge in data['edges']:
        u, v = index[edge[0]], index[edge[1]]
        succ[u].append(v)
        pred[v].append(u)
    indegree = [len(p) for p in pred]
    outdegree = [len(s) for s in succ]
    remaining = indegree[:]
    queue = deque(i for i in range(n) if remaining[i] == 0)
    order = []
    earliest = [0] * n
    while queue:
        u = queue.popleft()
        order.append(u)
        finish = earliest[u] + duration[u]
        for v in succ[u]:
            if finish > earliest[v]:
                earliest[v] = finish
            remaining[v] -= 1
            if remaining[v] == 0:
                queue.append(v)
    if len(order) != n:
        raise ValueError('Input graph is not a DAG')
    for k in range(nt):
        if durations[k] > 0 and capacities[k] <= 0:
            raise ValueError('Positive-duration operations require positive capacity')

    tail = [0] * n
    for u in reversed(order):
        tail[u] = duration[u] + max((tail[v] for v in succ[u]), default=0)
    critical_path = max(tail)

    def parallel_schedule(adj, degrees, priority, cutoff=None):
        """Event-driven non-delay list scheduling."""
        rem = degrees[:]
        ready = [[] for _ in range(nt)]
        for u in range(n):
            if rem[u] == 0:
                ready[kinds[u]].append((-priority[u], u))
        for h in ready:
            heapq.heapify(h)
        free = capacities[:]
        dirty = []
        marked = [False] * nt

        def mark(k):
            if (not marked[k] and ready[k]
                    and (free[k] > 0 or durations[k] == 0)):
                marked[k] = True
                heapq.heappush(dirty, k)

        def complete(u):
            for v in adj[u]:
                rem[v] -= 1
                if rem[v] == 0:
                    k = kinds[v]
                    heapq.heappush(ready[k], (-priority[v], v))
                    mark(k)

        for k in range(nt):
            mark(k)
        starts = [0] * n
        events = []
        now = 0
        span = 0
        scheduled = 0
        while scheduled < n:
            while dirty:
                k = heapq.heappop(dirty)
                marked[k] = False
                h = ready[k]
                d = durations[k]
                while h and (free[k] > 0 or d == 0):
                    if (cutoff is not None and scheduled % 64 == 0
                            and time.perf_counter() >= cutoff):
                        return None
                    _, u = heapq.heappop(h)
                    starts[u] = now
                    scheduled += 1
                    finish = now + d
                    if finish > span:
                        span = finish
                    if d == 0:
                        complete(u)
                    else:
                        free[k] -= 1
                        heapq.heappush(events, (finish, u))
            if scheduled == n:
                break
            if not events:
                raise ValueError('No feasible scheduling progress')
            now = events[0][0]
            while events and events[0][0] == now:
                _, u = heapq.heappop(events)
                k = kinds[u]
                free[k] += 1
                mark(k)
                complete(u)
        return span, starts

    def serial_schedule(adj, degrees, priority, cutoff):
        """Topological insertion scheduling using aggregate resource calendars."""
        rem = degrees[:]
        ready = [(-priority[u], u) for u in range(n) if rem[u] == 0]
        heapq.heapify(ready)
        release = [0] * n
        calendars = [[] for _ in range(nt)]
        starts = [0] * n
        span = 0
        placed = 0
        while ready:
            if placed % 32 == 0 and time.perf_counter() >= cutoff:
                return None
            _, u = heapq.heappop(ready)
            k = kinds[u]
            d = durations[k]
            c = capacities[k]
            t = release[u]
            if d:
                calendar = calendars[k]
                # Equal durations imply equally ordered starts and finishes.
                # Every saturated interval is generated by c consecutive starts:
                # [calendar[j+c-1], calendar[j]+d).
                j = bisect_right(calendar, t - d)
                limit = len(calendar) - c
                while j <= limit:
                    left = calendar[j + c - 1]
                    right = calendar[j] + d
                    if left >= t + d:
                        break
                    if left < right and right > t:
                        t = right
                    j += 1
                insort(calendar, t)
            starts[u] = t
            finish = t + d
            span = max(span, finish)
            placed += 1
            for v in adj[u]:
                if finish > release[v]:
                    release[v] = finish
                rem[v] -= 1
                if rem[v] == 0:
                    heapq.heappush(ready, (-priority[v], v))
        return span, starts

    # Establish and save a legal critical-path baseline before any search.
    best_span, best = parallel_schedule(succ, indegree, tail)
    best_sum = sum(best)
    write_solution(best)

    lower_bound = critical_path
    for k in range(nt):
        if durations[k]:
            lower_bound = max(lower_bound,
                              ((counts[k] + capacities[k] - 1) // capacities[k])
                              * durations[k])
    if best_span == lower_bound or time.perf_counter() >= deadline:
        return

    # Additional valid bounds use precedence-derived releases and delivery tails.
    releases_by_type = [[] for _ in range(nt)]
    tails_by_type = [[] for _ in range(nt)]
    for u in range(n):
        k = kinds[u]
        releases_by_type[k].append(earliest[u])
        tails_by_type[k].append(tail[u] - duration[u])
    for k in range(nt):
        d, c = durations[k], capacities[k]
        if not d:
            continue
        for values in (releases_by_type[k], tails_by_type[k]):
            values.sort(reverse=True)
            for j, value in enumerate(values, 1):
                lower_bound = max(lower_bound, value + ((j + c - 1) // c) * d)
    if best_span == lower_bound or time.perf_counter() >= deadline:
        return

    reverse_tail = [earliest[u] + duration[u] for u in range(n)]
    reverse_head = [tail[u] - duration[u] for u in range(n)]
    norm = max(1, critical_path)
    tails = [[x / norm for x in tail],
             [x / norm for x in reverse_tail]]
    heads = [[x / norm for x in earliest],
             [x / norm for x in reverse_head]]

    # Distributed descendant work accounts for fanout without exponentially
    # counting shared subgraphs. Compute the analogous feature in reverse too.
    work_features = []
    for adj, degrees, traversal in ((succ, indegree, reversed(order)),
                                    (pred, outdegree, iter(order))):
        work = [0.0] * n
        for u in traversal:
            work[u] = duration[u] / max(1, capacities[kinds[u]])
            for v in adj[u]:
                work[u] += work[v] / max(1, degrees[v])
        scale = max(1.0, max(work))
        work_features.append([x / scale for x in work])

    rng = random.Random(1729)
    allow_serial = n <= 1000

    def run(priority, backward, use_serial):
        if time.perf_counter() >= deadline:
            return None
        adj, degrees = (pred, outdegree) if backward else (succ, indegree)
        if use_serial and allow_serial:
            result = serial_schedule(adj, degrees, priority, deadline)
        else:
            result = parallel_schedule(adj, degrees, priority, deadline)
        if result is None:
            return None
        span, starts = result
        if backward:
            starts = [span - starts[u] - duration[u] for u in range(n)]
        return span, starts

    def consider(result):
        nonlocal best_span, best, best_sum
        if result is None:
            return False
        span, starts = result
        if span > best_span:
            return False
        total = sum(starts)
        improved = span < best_span
        if improved or (span == best_span and total < best_sum):
            best_span, best, best_sum = span, starts, total
        return improved

    def justify(starts):
        # Schedule latest-finishing operations first in the reversed graph,
        # mirror back, then schedule earliest-starting operations first forward.
        priority = [starts[u] + duration[u] for u in range(n)]
        backward = run(priority, True, True)
        if backward is None:
            return
        consider(backward)
        priority = [-s for s in backward[1]]
        consider(run(priority, False, True))

    justify(best)
    limit = 12000 if n <= 100 else 3000
    for iteration in range(limit):
        if best_span == lower_bound or time.perf_counter() >= deadline:
            break
        backward = bool((iteration // 2) % 2)
        direction = int(backward)
        use_serial = bool(iteration % 2)
        family = (iteration // 4) % 8
        b = tails[direction]
        a = heads[direction]
        w = work_features[direction]
        noisy = iteration >= 32
        noise = rng.choice((0.015, 0.04, 0.1, 0.25)) if noisy else 0.0

        if family == 0:
            priority = [b[u] + 0.08 * w[u] for u in range(n)]
        elif family == 1:
            priority = [b[u] + 0.6 * w[u] - 0.15 * a[u]
                        for u in range(n)]
        elif family == 2:
            priority = [w[u] + 0.35 * b[u] - 0.1 * a[u]
                        for u in range(n)]
        elif family == 3:
            priority = [-a[u] + 0.2 * b[u] + 0.1 * w[u]
                        for u in range(n)]
        elif family == 4:
            weight = rng.choice((0.0, 0.2, 0.5, 1.0, 2.0))
            priority = [b[u] + weight * w[u] for u in range(n)]
            noise = max(noise, 0.08)
        elif family in (5, 6):
            scale = max(1, best_span)
            if backward:
                priority = [(best[u] + duration[u]) / scale
                            for u in range(n)]
            else:
                priority = [-best[u] / scale for u in range(n)]
            amplitude = rng.choice((0.025, 0.06, 0.15, 0.35))
            selected_type = rng.randrange(nt)
            for u in range(n):
                if family == 5 or kinds[u] == selected_type:
                    priority[u] += amplitude * (rng.random() - 0.5)
                priority[u] += 0.0001 * b[u]
        else:
            wb = rng.uniform(0.2, 1.5)
            ww = rng.uniform(0.0, 1.5)
            wa = rng.uniform(-0.4, 0.4)
            priority = [wb * b[u] + ww * w[u] + wa * a[u]
                        for u in range(n)]
            noise = max(noise, 0.04)
        if noise:
            for u in range(n):
                priority[u] += noise * (rng.random() - 0.5)

        result = run(priority, backward, use_serial)
        if result is None:
            break
        improved = consider(result)
        if best_span == lower_bound:
            break
        if improved or iteration % 24 == 23:
            justify(best)

    write_solution(best)

