"""Write a compact, reviewable record from the complete-program scheduling experiments."""
import argparse
from collections import Counter
from pathlib import Path
from statistics import geometric_mean

from flow_pilot import read_json, write_json
from paper_order_flow import hashfile


def counts(pairs):
    return {"paired":len(pairs),"faster":sum(a<b for a,b in pairs),"tie":sum(a==b for a,b in pairs),
            "slower":sum(a>b for a,b in pairs),"geomean_ratio":geometric_mean(a/b for a,b in pairs) if pairs else None}


def main():
    cli=argparse.ArgumentParser(description=__doc__)
    for name in ("verified","joint","order","loops","output"):
        cli.add_argument("--"+name,type=Path,required=True)
    a=cli.parse_args()
    complete=read_json(a.verified/"results.json")
    joint_run=read_json(a.joint/"results.json")
    if joint_run["parent_run"] != hashfile(a.verified/"results.json"):
        raise ValueError("joint policies were measured on a different baseline")
    joint=joint_run["cases"]
    protocol=complete["protocol"]
    if set(complete["cases"]) != set(protocol["dev"]+protocol["holdout"]) or set(joint)!=set(complete["cases"]):
        raise ValueError("incomplete comparison")
    rows={}
    for n,r in complete["cases"].items():
        native=r["native"]
        row={"split":r["split"],"native_status":native["status"],"native":native.get("cycles"),
             "observed_checksum":native.get("checksum"),
             "native_diagnostic":native.get("diagnostic"),"published":{k:int(v) for k,v in r["published"].items()
                                                                        if k!="design"}}
        for group in ("manual","agent"):
            if group not in r:
                continue
            for k in ("ordering","pipeline","deployed"):
                x=r[group][k]
                row[group+"_"+k]=x.get("cycles") if x["status"]=="valid" else None
            x=r[group]["deployed"]
            row[group+"_choice"]=x.get("choice")
            row[group+"_deployed_status"]=x["status"]
            row[group+"_pipeline_attempts"]=r[group]["pipeline"]["attempts"]
            x=joint[n][group]
            row[group+"_joint"]=x.get("cycles") if x["status"]=="valid" else None
            row[group+"_joint_diagnosis"]=x
        rows[n]=row
    groups={}
    for split in ("dev","holdout","all"):
        selected=[r for r in rows.values() if split=="all" or r["split"]==split]
        result={"total":len(selected), "native_status":dict(Counter(r["native_status"] for r in selected))}
        for metric in ("native","manual_ordering","agent_ordering","manual_deployed","agent_deployed","manual_joint","agent_joint"):
            for target in ("native","manual_deployed","manual_joint","vericert-hyperblock","bambu-opt","best-published"):
                pairs=[]
                for row in selected:
                    x=row.get(metric)
                    if target=="best-published":
                        y=min(row["published"].values())
                    else:
                        y=row["published"].get(target,row.get(target))
                    if x is not None and y is not None:
                        pairs.append((x,y))
                result[f"{metric}/{target}"]=counts(pairs)
        groups[split]=result
    receipts={}
    for name,path in [("block",a.order),("loop",a.loops)]:
        calls=[read_json(c) for c in sorted(path.glob("model_*/call.json"))]
        receipts[name]={"calls":len(calls),"status":[x["status"] for x in calls],
                        "tokens":dict(sum((Counter(x["usage"]) for x in calls), Counter()))}
    order=read_json(a.order/"frozen.json")
    loops=read_json(a.loops/"dev.json")
    write_json(a.output,{"protocol":complete["protocol"],"model_receipts":receipts,"summary":groups,
                        "order_search":order,"loop_freeze":read_json(a.loops/"frozen.json"),
                        "loop_search":{label:{"weights":x["weights"],"score":x["score"]}
                                                          for label,x in loops.items()},"cases":rows})
    for split,g in groups.items():
        print(split,g["native_status"])
        for k in ("agent_deployed/native","agent_deployed/manual_deployed","agent_joint/native","agent_joint/manual_joint",
                  "agent_joint/vericert-hyperblock","agent_joint/best-published"):
            print(k,g[k])


if __name__=="__main__":
    main()
