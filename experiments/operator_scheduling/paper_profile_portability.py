"""Transfer frozen policy decisions to a second compiler without new model or promotion calls."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from flow_pilot import read_json, write_json
from paper_order_flow import BOARD_PATH, hashfile
from paper_profile_flow import FEATURES, measure, profile, summary


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for key in (
        "source-base",
        "target-base",
        "first-result",
        "residual-result",
        "compiler",
        "run",
        "summary",
    ):
        p.add_argument("--" + key, type=Path, required=True)
    p.add_argument("--library", type=Path, default=BOARD_PATH)
    p.add_argument("--jobs", type=int, default=6)
    a = p.parse_args()
    source, target, compiler, lib, run = [
        d.resolve() for d in (a.source_base, a.target_base, a.compiler, a.library, a.run)
    ]
    previous = read_json(source / "protocol.json")
    current = read_json(target / "protocol.json")
    first = read_json(a.first_result)["frozen"]
    residual = read_json(a.residual_result)["frozen"]
    for key in ("source_hashes", "dev", "holdout", "board_sha256", "period_ns", "part"):
        if previous[key] != current[key]:
            raise ValueError(f"cannot compare differing {key}")
    if not (previous["compiler_sha256"] == first["compiler"] == residual["protocol"]["compiler"]):
        raise ValueError("source policies were trained on another compiler")
    if (
        current["compiler_sha256"] == previous["compiler_sha256"]
        or hashfile(compiler) != current["compiler_sha256"]
        or any(hashfile(lib / k) != v for k, v in current["board_sha256"].items())
    ):
        raise ValueError("target compiler transition or board mismatch")
    if (
        first["features"] != FEATURES
        or first["dev"] != current["dev"]
        or residual["deploy"] != "manual_incumbent"
    ):
        raise ValueError("unexpected frozen feature order, split or deployed residual rule")
    dev, hold = current["dev"], current["holdout"]
    names = dev + hold
    run.mkdir(parents=True, exist_ok=True)
    transfer = {
        "posthoc_compiler_portability": True,
        "source_sha256": previous["compiler_sha256"],
        "target_sha256": current["compiler_sha256"],
        "source_protocol_sha256": hashfile(source / "protocol.json"),
        "target_protocol_sha256": hashfile(target / "protocol.json"),
        "original_raw_archive_sha256": hashfile(a.first_result),
        "original_residual_archive_sha256": hashfile(a.residual_result),
        "frozen_weights": first["weights"],
        "source_model_prompt_sha256": first["prompt_sha256"],
        "feature_order": FEATURES,
        "source_residual_deployment": residual["deploy"],
        "part": current["part"],
        "period_ns": current["period_ns"],
        "dev": dev,
        "holdout": hold,
        "max_probes_per_case_policy": 3,
        "new_model_calls": 0,
    }
    persisted = run / "protocol.json"
    if persisted.is_file() and read_json(persisted) != transfer:
        raise ValueError("existing run was created with different transfer conditions")
    write_json(persisted, transfer)
    for n in names:
        if hashfile(target / "input" / n / "kernel.cpp") != current["source_hashes"][n]["adapter"]:
            raise ValueError(f"adapter changed for {n}")
    with ThreadPoolExecutor(max_workers=a.jobs) as pool:
        profiles = dict(zip(names, pool.map(lambda n: profile(n, target, run), names), strict=True))

    def apply(label, weights):
        with ThreadPoolExecutor(max_workers=a.jobs) as pool:
            rows = dict(
                zip(
                    names,
                    pool.map(
                        lambda n: measure(
                            n, target, run, compiler, lib, profiles, weights, remeasure=True
                        ),
                        names,
                    ),
                    strict=True,
                )
            )
        write_json(run / (label + ".json"), rows)
        return rows

    manual = apply("manual", None)
    raw = apply("raw_v1", first["weights"])
    splits = {}
    for label, subset in [("dev", dev), ("holdout", hold), ("all", names)]:
        splits[label] = {
            "raw_v1": summary(subset, profiles, manual, raw),
            "deployed_residual": summary(subset, profiles, manual, manual),
        }
    results = {}
    for n in names:
        native = profiles[n]

        def compact(row):
            return {
                **{k: v for k, v in row.items() if k != "attempts"},
                "attempts": {
                    l: {
                        "source": v["source"],
                        "status": v["probe"]["status"],
                        "predicted_saving": v["predicted_saving"],
                    }
                    for l, v in row.get("attempts", {}).items()
                },
            }

        results[n] = {
            "split": "dev" if n in dev else "holdout",
            "native_cycles": native.get("native_cycles"),
            "checksum": native.get("checksum"),
            "native_rtl_sha256": native.get("rtl_sha256"),
            "loops": len(native.get("loops", {})),
            "mapped": sum(int(x["features"][-1]) for x in native.get("loops", {}).values()),
            "manual": compact(manual[n]),
            "raw_v1": compact(raw[n]),
            "deployed_residual_cycles": manual[n].get("cycles"),
        }
    write_json(a.summary, {"transfer": transfer, "splits": splits, "cases": results})


if __name__ == "__main__":
    main()
