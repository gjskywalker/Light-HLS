"""Summarize the three-group factorial within the same Bambu release in log cycles."""
import argparse
import hashlib
from itertools import permutations
import json
from math import exp, isclose, log
from pathlib import Path
import re
from statistics import mean, geometric_mean


CORNERS = {"": "default", "F": "F", "S": "S", "M": "no_async_memory",
           "FS":"FS", "FM":"FM", "SM":"SM", "FSM":"paper_noopt"}


def design_digest(folder):
    verilog = (folder/"main_top.v").read_text()
    # Remove generator banner only. Replace contents of RAM filename strings
    # with the digest of the actual initialization data consumed by simulation.
    verilog = re.sub(r"^// (?:Code created using PandA|.* executed with:).*\n", "", verilog, flags=re.M)
    def memory_digest(match):
        filename = folder / match[1]
        digest = hashlib.sha256(filename.read_bytes()).hexdigest()
        return f'.MEMORY_INIT_file("{digest}")'
    verilog = re.sub(r'\.MEMORY_INIT_file\("([^"\n]+\.mem)"\)', memory_digest, verilog)
    return hashlib.sha256(verilog.encode()).hexdigest()


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    for key in ("matrix","factorial","output"):
        cli.add_argument("--"+key, required=True, type=Path)
    cli.add_argument("--matrix-run", type=Path)
    cli.add_argument("--priority-run", type=Path)
    cli.add_argument("--priority-results", type=Path)
    cli.add_argument("--archive-audit", type=Path,
                     help="compare each configuration with the frozen Light-HLS Agent")
    a = cli.parse_args()
    matrix, factorial = [json.loads(path.read_text()) for path in (a.matrix,a.factorial)]
    for key in ("revision","bambu_binary_sha256","archive_sha256"):
        if matrix[key] != factorial[key]:
            raise ValueError(f"different {key}")
    if set(matrix["cases"]) != set(factorial["cases"]):
        raise ValueError("case sets differ")
    rows = {}
    for name, main_rows in matrix["cases"].items():
        cases = dict(main_rows, **factorial["cases"][name])
        cy = {frozenset(k):cases[v]["cycles"] for k,v in CORNERS.items() if cases[v]["status"]=="valid"}
        if len(cy)!=8:
            raise ValueError(f"incomplete factorial for {name}")
        contrib = {k:[] for k in "FSM"}
        for order in permutations("FSM"):
            off = frozenset()
            for k in order:
                nxt = off | {k}
                contrib[k].append(log(cy[nxt]/cy[off]))
                off = nxt
        shapley = {k:mean(values) for k,values in contrib.items()}
        total = log(cy[frozenset("FSM")]/cy[frozenset()])
        if not isclose(sum(shapley.values()), total, abs_tol=1e-12):
            raise ValueError("log decomposition failed")
        rows[name] = {"corner_cycles":{k:cy[frozenset(k)] for k in CORNERS},
                      "mean_marginal_log_cycle_cost":shapley}
    mean_logs = {k:mean(row["mean_marginal_log_cycle_cost"][k] for row in rows.values()) for k in "FSM"}
    span = sum(mean_logs.values())
    aggregates = {"cases":len(rows),"noopt_over_default":exp(span),
                  "share_of_endpoint_log_cycle_gap":{k:val/span for k,val in mean_logs.items()},
                  "multiplicative_cost_by_group":{k:exp(val) for k,val in mean_logs.items()}}
    # The range shows how much the estimate depends on the other two switches.
    for knob in "FSM":
        ratios = []
        for corner in CORNERS:
            if knob not in corner:
                target = next(k for k in CORNERS if set(k) == set(corner)|{knob})
                ratios.append(geometric_mean(r["corner_cycles"][target]/r["corner_cycles"][corner]
                                             for r in rows.values()))
        aggregates.setdefault("marginal_geomean_range",{})[knob] = [min(ratios),max(ratios)]
    result = {"groups":{"F":"-O0 and -fno-if-conversion", "S":"disable both general and memory chaining",
                        "M":"exclude asynchronous memories from allocation"},
              "summary":aggregates, "cases":rows}
    arms = [matrix, factorial]
    if any((a.matrix_run, a.priority_run, a.priority_results)):
        if not all((a.matrix_run, a.priority_run, a.priority_results)):
            cli.error("priority checks require both run directories and the priority result file")
        extra = json.loads(a.priority_results.read_text())
        arms.append(extra)
        for key in ("revision","bambu_binary_sha256","archive_sha256"):
            if matrix[key] != extra[key]:
                raise ValueError(f"priority runs differ in {key}")
        for arm, source, directory in [("static_mobility",matrix,a.matrix_run),
                                      ("fixed_priority",extra,a.priority_run),
                                      ("single_channel",extra,a.priority_run)]:
            pairs = [(source["cases"][n][arm]["cycles"],matrix["cases"][n]["default"]["cycles"])
                     for n in rows]
            matches = [n for n in rows if design_digest(directory/n/arm) ==
                        design_digest(a.matrix_run/n/"default")]
            result.setdefault("priority_and_channels",{})[arm] = {
                "pairs":len(pairs),"fewer":sum(x<y for x,y in pairs),"equal":sum(x==y for x,y in pairs),
                "more":sum(x>y for x,y in pairs),"ratio":geometric_mean(x/y for x,y in pairs),
                "normalized_rtl_and_initialization_match":matches}
    if a.archive_audit:
        audit = json.loads(a.archive_audit.read_text())
        if audit["archive_sha256"] != matrix["archive_sha256"]:
            raise ValueError("archive hash differs from the cross-tool reference")
        frozen = {name.replace("-", "_"): row["agent_cycles"] for name, row in audit["cases"].items()}
        if set(rows) != set(frozen):
            raise ValueError("case set differs from the cross-tool reference")
        for arm_set in arms:
            for arm in arm_set["flags"]:
                pairs = [(case[arm]["cycles"], frozen[name]) for name, case in arm_set["cases"].items()
                         if case[arm]["status"] == "valid"]
                result.setdefault("bambu_vs_frozen_agent", {})[arm] = {
                    "pairs":len(pairs), "bambu_fewer":sum(x<y for x,y in pairs),
                    "equal":sum(x==y for x,y in pairs), "bambu_more":sum(x>y for x,y in pairs),
                    "geomean_bambu_over_light":geometric_mean(x/y for x,y in pairs)}
    a.output.write_text(json.dumps(result,indent=2)+"\n")
    print(json.dumps(aggregates,indent=2))


if __name__ == "__main__":
    main()
