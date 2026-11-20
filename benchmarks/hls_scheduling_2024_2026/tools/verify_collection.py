#!/usr/bin/env python3
"""Verify collected files and graph structure without running upstream code."""

import argparse
import collections
import hashlib
import json
from pathlib import Path
import re


BASE=Path(__file__).resolve().parents[1]


def sha256(path):
    digest=hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda:source.read(1024*1024),b''):
            digest.update(block)
    return digest.hexdigest()


def check_dag(nodes,edges):
    if len(nodes)!=len(set(nodes)):
        raise ValueError('duplicate node identifiers')
    known=set(nodes)
    successors={node:[] for node in nodes}
    indegree=dict.fromkeys(nodes,0)
    for source,target in edges:
        if source not in known or target not in known:
            raise ValueError('edge references an unknown node')
        successors[source].append(target)
        indegree[target]+=1
    ready=collections.deque(node for node in nodes if not indegree[node])
    visited=0
    while ready:
        node=ready.popleft();visited+=1
        for target in successors[node]:
            indegree[target]-=1
            if not indegree[target]:ready.append(target)
    if visited!=len(nodes):
        raise ValueError('graph contains a cycle')


def inspect_graph(path):
    if path.suffix=='.json':
        data=json.loads(path.read_text())
        nodes=[item[0] for item in data['nodes']]
        edges=[item[:2] for item in data['edges']]
        if not nodes:raise ValueError('empty graph')
        for node,kind in data['nodes']:
            if not isinstance(data['delay'][kind],int) or data['delay'][kind]<=0:
                raise ValueError('invalid operation duration')
            if not isinstance(data['resource'][kind],int) or data['resource'][kind]<=0:
                raise ValueError('invalid resource capacity')
        kind='HeuriGym DAG; durations/capacities positive; scheduling not executed'
    elif path.suffix=='.gml':
        text=path.read_text()
        # The collected GML dialect has scalar node/edge attributes, no nested graphics.
        blocks=re.findall(r'\bnode\s*\[([^\[\]]*)\]',text,re.S)
        edge_blocks=re.findall(r'\bedge\s*\[([^\[\]]*)\]',text,re.S)
        if len(blocks)!=len(re.findall(r'\bnode\s*\[',text)):
            raise ValueError('unsupported nested GML node attributes')
        if len(edge_blocks)!=len(re.findall(r'\bedge\s*\[',text)):
            raise ValueError('unsupported nested GML edge attributes')
        nodes=[int(re.search(r'\bid\s+(-?\d+)',block).group(1)) for block in blocks]
        edges=[(int(re.search(r'\bsource\s+(-?\d+)',block).group(1)),
                int(re.search(r'\btarget\s+(-?\d+)',block).group(1))) for block in edge_blocks]
        if not nodes or not re.search(r'\bdirected\s+1\b',text):
            raise ValueError('not a nonempty directed graph')
        kind='Collected scalar-attribute GML DAG; resource model not executed'
    else:
        raise ValueError('unknown graph file format')
    check_dag(nodes,edges)
    return {'nodes':len(nodes),'edges':len(edges),'scope':kind}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,help='Optional JSON report path')
    args=parser.parse_args()
    errors=[]
    papers=json.loads((BASE/'papers.json').read_text())
    artifacts=json.loads((BASE/'artifacts.json').read_text())
    cases=json.loads((BASE/'cases.json').read_text())
    files=json.loads((BASE/'files.json').read_text())['files']
    paper_ids={row['id'] for row in papers}
    artifact_ids={row['id'] for row in artifacts}
    indexed={row['path']:row for row in files}
    checked=0
    for row in files:
        path=(BASE/row['path']).resolve()
        if not path.is_relative_to(BASE):
            errors.append({'file':row['path'],'error':'path escapes collection'});continue
        if not path.is_file():
            errors.append({'file':row['path'],'error':'missing file'});continue
        if sha256(path)!=row['sha256'] or path.stat().st_size!=row['bytes']:
            errors.append({'file':row['path'],'error':'hash or size mismatch'})
        checked+=1
    if len({row['id'] for row in cases})!=len(cases):
        errors.append({'error':'duplicate case identifiers'})
    graphs=[]
    for row in cases:
        if row['artifact_id'] not in artifact_ids or not set(row['paper_ids'])<=paper_ids:
            errors.append({'case':row['id'],'error':'invalid source relationship'})
        if row['source_path'] not in indexed or indexed[row['source_path']]['sha256']!=row['sha256']:
            errors.append({'case':row['id'],'error':'primary source not covered by file manifest'})
        for path in row['bundle_files']:
            if path not in indexed:errors.append({'case':row['id'],'error':'dependency not indexed','path':path})
        if row['track']=='graph_auxiliary':
            try:
                graphs.append({'case_id':row['id'],'sha256':row['sha256'],
                               'status':'valid',**inspect_graph(BASE/row['source_path'])})
            except (ValueError,KeyError,TypeError,AttributeError) as error:
                errors.append({'case':row['id'],'error':str(error)})
    report={'valid':not errors,'files_checked':checked,'cases':len(cases),'papers':len(papers),
            'graph_checks':graphs,'errors':errors,
            'scope':'Original-file integrity and graph structural checks only; no HLS/RTL correctness or paper QoR reproduction.'}
    if args.output:
        args.output.parent.mkdir(parents=True,exist_ok=True)
        args.output.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
    print(json.dumps({key:value for key,value in report.items() if key!='graph_checks'},ensure_ascii=False,indent=2))
    return 0 if report['valid'] else 1


if __name__=='__main__':
    raise SystemExit(main())
