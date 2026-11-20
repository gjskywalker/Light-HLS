#!/usr/bin/env python3
"""Run original Vericert initialization/checksum drivers under ASan/UBSan.

This is a native execution smoke check, not independent functional verification.
"""

import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


BASE=Path(__file__).resolve().parents[1]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--case',action='append',help='Optional exact case ID; repeatable')
    parser.add_argument('--clang',default='clang')
    parser.add_argument('--jobs',type=int,default=4)
    parser.add_argument('--timeout',type=int,default=20)
    args=parser.parse_args()
    if args.jobs<1 or args.timeout<1:parser.error('jobs and timeout must be positive')
    output=args.output.resolve()
    if output.exists() and any(output.iterdir()):parser.error('output must be absent or empty')
    cases=[c for c in json.loads((BASE/'cases.json').read_text()) if 'hyperblock_pldi2024' in c['paper_ids']]
    selected=set(args.case or [c['id'] for c in cases])
    if selected-{c['id'] for c in cases}:parser.error('unknown case ID')
    cases=[c for c in cases if c['id'] in selected]
    output.mkdir(parents=True,exist_ok=True)
    with (output/'integrity.log').open('w') as log:
        result=subprocess.run(['python3',str(BASE/'tools/verify_collection.py')],stdout=log,stderr=subprocess.STDOUT)
    if result.returncode:parser.error('collection integrity check failed; see integrity.log')

    def check(case):
        folder=output/case['id'];folder.mkdir()
        source=BASE/case['source_path']
        assert hashlib.sha256(source.read_bytes()).hexdigest()==case['sha256']
        command=[args.clang,'-std=c99','-O0','-g','-Wno-unknown-pragmas',
                 '-fsanitize=address,undefined','-fno-sanitize-recover=all',str(source),'-lm','-o',str(folder/'native')]
        row={'case_id':case['id'],'source':case['source_path'],'source_sha256':case['sha256'],
             'compile_command':command,'independent_oracle':False}
        started=time.monotonic()
        with (folder/'compile.log').open('w') as log:
            try:process=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,timeout=60)
            except subprocess.TimeoutExpired:row['status']='compile_timeout';return row
        row['compile_returncode']=process.returncode
        if process.returncode:row['status']='compile_failed';return row
        with (folder/'run.log').open('w') as log:
            try:
                process=subprocess.run([str(folder/'native')],stdout=log,stderr=subprocess.STDOUT,
                    timeout=args.timeout,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
            except subprocess.TimeoutExpired:row['status']='execution_timeout';return row
        text=(folder/'run.log').read_text(errors='replace')
        row.update(run_returncode=process.returncode,output=text[-2000:],elapsed_seconds=round(time.monotonic()-started,3))
        if 'runtime error:' in text or 'ERROR: AddressSanitizer' in text:row['status']='sanitizer_rejected'
        elif process.returncode<0:row['status']='execution_signal'
        else:row['status']='execution_completed_no_sanitizer_diagnostic'
        row['exit_note']='Original main may return a checksum; a nonzero normal exit is not automatically an oracle failure.'
        return row

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results=list(pool.map(check,cases))
    report={'scope':'native smoke only; no independent oracle, HLS or RTL equivalence',
            'asan_options':'detect_leaks=0','results':results}
    (output/'results.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
    print(json.dumps({r['case_id']:r['status'] for r in results},ensure_ascii=False,indent=2))
    return 0 if all(r['status']=='execution_completed_no_sanitizer_diagnostic' for r in results) else 1


if __name__=='__main__':
    raise SystemExit(main())
