import json
import heapq
import bisect
import random
import time


class _SearchTimeout(Exception):
    pass


def solve(input_file: str, solution_file: str):
    began = time.monotonic()
    deadline = began + 8.4
    with open(input_file, 'r', encoding='utf-8') as f:
        data = json.load(f)

    nodes = data['nodes']
    n = len(nodes)
    if not n:
        with open(solution_file, 'w', encoding='utf-8'):
            pass
        return

    ids = [v[0] for v in nodes]
    index = {v: i for i, v in enumerate(ids)}
    type_index = {}
    kinds = []
    durations_by_type = []
    capacities = []
    for _, kind in nodes:
        if kind not in type_index:
            type_index[kind] = len(type_index)
            durations_by_type.append(int(data['delay'][kind]))
            capacities.append(int(data['resource'][kind]))
        kinds.append(type_index[kind])
    nt = len(capacities)
    counts = [0] * nt
    for k in kinds:
        counts[k] += 1
    for k in range(nt):
        if durations_by_type[k] == 0:
            capacities[k] = max(1, capacities[k])
        if capacities[k] <= 0:
            raise ValueError('An operation with positive delay needs capacity')
        capacities[k] = min(capacities[k], counts[k])
    dur = [durations_by_type[k] for k in kinds]

    succ = [[] for _ in range(n)]
    pred = [[] for _ in range(n)]
    # Parallel edges express the same precedence constraint.
    seen = set()
    for edge in data['edges']:
        u, v = index[edge[0]], index[edge[1]]
        if (u, v) not in seen:
            seen.add((u, v))
            succ[u].append(v)
            pred[v].append(u)
    indeg = [len(p) for p in pred]
    remaining = indeg[:]
    queue = [i for i in range(n) if remaining[i] == 0]
    topo = []
    head = [0] * n
    for u in queue:
        topo.append(u)
        finish = head[u] + dur[u]
        for v in succ[u]:
            if finish > head[v]:
                head[v] = finish
            remaining[v] -= 1
            if remaining[v] == 0:
                queue.append(v)
    if len(topo) != n:
        raise ValueError('The input graph is not a DAG')

    tail = [0] * n
    for u in reversed(topo):
        tail[u] = dur[u] + max((tail[v] for v in succ[u]), default=0)
    reverse_tail = [head[i] + dur[i] for i in range(n)]
    cp = max(tail)
    lower_bound = cp
    loads = []
    for k in range(nt):
        c = capacities[k]
        d = durations_by_type[k]
        lower_bound = max(lower_bound, ((counts[k] + c - 1) // c) * d)
        loads.append((counts[k] * d + c - 1) // c)

    def write_solution(starts):
        with open(solution_file, 'w', encoding='utf-8') as f:
            f.writelines(str(ids[i]) + ':' + str(starts[i]) + '\n'
                         for i in range(n))

    def check_time():
        if time.monotonic() >= deadline:
            raise _SearchTimeout()

    def event_schedule(keys, reverse=False, bounded=True):
        outgoing = pred if reverse else succ
        degree = ([len(s) for s in succ] if reverse else indeg[:])
        ready = [[] for _ in range(nt)]
        touched = set()
        for i in range(n):
            if degree[i] == 0:
                k = kinds[i]
                heapq.heappush(ready[k], (-keys[i], i))
                touched.add(k)
        free = capacities[:]
        running = []
        starts = [0] * n
        now = 0
        done = 0
        ticks = 0
        while done < n:
            for k in sorted(touched):
                h = ready[k]
                while free[k] and h:
                    _, u = heapq.heappop(h)
                    starts[u] = now
                    free[k] -= 1
                    heapq.heappush(running, (now + dur[u], u))
            touched.clear()
            if not running:
                raise ValueError('No feasible schedule')
            now = running[0][0]
            while running and running[0][0] == now:
                _, u = heapq.heappop(running)
                done += 1
                k = kinds[u]
                free[k] += 1
                touched.add(k)
                for v in outgoing[u]:
                    degree[v] -= 1
                    if degree[v] == 0:
                        j = kinds[v]
                        heapq.heappush(ready[j], (-keys[v], v))
                        touched.add(j)
                ticks += 1
                if bounded and ticks % 256 == 0:
                    check_time()
        return starts, now

    # Establish and save a complete legal incumbent before doing any search.
    initial_keys = [tail[i] * (n + 1) + len(succ[i]) for i in range(n)]
    best, best_span = event_schedule(initial_keys, bounded=False)
    best_sum = sum(best)
    write_solution(best)
    if best_span == lower_bound or time.monotonic() >= deadline:
        return

    def serial_schedule(keys, reverse=False):
        outgoing = pred if reverse else succ
        degree = ([len(s) for s in succ] if reverse else indeg[:])
        ready = [(-keys[i], i) for i in range(n) if degree[i] == 0]
        heapq.heapify(ready)
        release = [0] * n
        starts = [0] * n
        calendars = [[[] for _ in range(capacities[k])] for k in range(nt)]
        horizon = 0
        steps = 0
        operations = 0
        while ready:
            _, u = heapq.heappop(ready)
            r, d = release[u], dur[u]
            if d == 0:
                s = r
            else:
                chosen = None
                chosen_pos = 0
                earliest = None
                for calendar in calendars[kinds[u]]:
                    pos = bisect.bisect_right(calendar, (r, float('inf')))
                    s = r
                    if pos and calendar[pos - 1][1] > s:
                        s = calendar[pos - 1][1]
                    while pos < len(calendar) and s + d > calendar[pos][0]:
                        s = max(s, calendar[pos][1])
                        pos += 1
                        operations += 1
                        if operations % 512 == 0:
                            check_time()
                    if earliest is None or s < earliest:
                        earliest = s
                        chosen = calendar
                        chosen_pos = pos
                    operations += 1
                    if operations % 512 == 0:
                        check_time()
                    if earliest == r:
                        break
                s = earliest
                chosen.insert(chosen_pos, (s, s + d))
            starts[u] = s
            finish = s + d
            horizon = max(horizon, finish)
            for v in outgoing[u]:
                if finish > release[v]:
                    release[v] = finish
                degree[v] -= 1
                if degree[v] == 0:
                    heapq.heappush(ready, (-keys[v], v))
            steps += 1
            if steps % 64 == 0:
                check_time()
        return starts, horizon

    # Resource-weighted path ranks favor chains feeding heavily used resources.
    max_load = max(1, max(loads))
    weight = [dur[i] * (1024 + 2048 * loads[kinds[i]] // max_load)
              for i in range(n)]
    weighted_forward = [0] * n
    weighted_reverse = [0] * n
    for u in reversed(topo):
        weighted_forward[u] = weight[u] + max(
            (weighted_forward[v] for v in succ[u]), default=0)
    for u in topo:
        weighted_reverse[u] = weight[u] + max(
            (weighted_reverse[v] for v in pred[u]), default=0)
    scale = max(1, cp)
    for ranks in (weighted_forward, weighted_reverse):
        largest = max(1, max(ranks))
        for i in range(n):
            ranks[i] = ranks[i] * scale * 1024 // largest

    rng = random.Random(1729)
    use_serial = n <= 900 and len(seen) <= 20000
    max_trials = min(6000, max(24, 180000 // max(1, n)))
    try:
        for trial in range(max_trials):
            check_time()
            phase = trial % 12
            reverse = phase % 2 == 0
            direction_tail = reverse_tail if reverse else tail
            if phase < 4:
                # Backward/forward justification guided by the incumbent.
                if reverse:
                    base = [(best[i] + dur[i]) * 1024 for i in range(n)]
                else:
                    base = [(best_span - best[i]) * 1024 for i in range(n)]
            elif phase < 8:
                base = [v * 1024 for v in direction_tail]
            else:
                ranks = weighted_reverse if reverse else weighted_forward
                blend = rng.randrange(257)
                base = [(direction_tail[i] * 1024 * blend
                         + ranks[i] * (256 - blend)) // 256
                        for i in range(n)]
            if phase in (0, 1, 4, 5, 8, 9) and trial < 12:
                amplitude = 0
            elif phase in (0, 1):
                amplitude = 0
            else:
                amplitude = scale * rng.choice((16, 32, 64, 128, 256, 512, 1024))
            if amplitude:
                keys = [b + rng.randrange(-amplitude, amplitude + 1) for b in base]
            else:
                keys = base
            # Include non-delay schedules among the insertion-based candidates.
            if use_serial and trial % 5 != 4:
                candidate, span = serial_schedule(keys, reverse)
            else:
                candidate, span = event_schedule(keys, reverse)
            if reverse:
                candidate = [span - candidate[i] - dur[i] for i in range(n)]
            total = sum(candidate)
            if (span, total) < (best_span, best_sum):
                best, best_span, best_sum = candidate, span, total
                if best_span == lower_bound:
                    break
    except _SearchTimeout:
        pass
    write_solution(best)


if __name__ == '__main__':
    import sys
    solve(sys.argv[1], sys.argv[2])

