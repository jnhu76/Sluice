"""Observe removed options/resource rejection before any target-file open."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

ap=argparse.ArgumentParser();ap.add_argument('app',choices=['tail','copy']);ap.add_argument('binary',type=Path);ap.add_argument('output',type=Path);args=ap.parse_args();args.output.mkdir(parents=True,exist_ok=False)
binary=args.binary.resolve();records=[];failures=[]
with tempfile.TemporaryDirectory(prefix='sluice-preflight-oracle-') as directory:
 d=Path(directory);source=d/'source';source.write_text('seed\n');dest=d/'destination';dest.write_text('sentinel\n');missing=d/'missing'
 cases=[]
 if args.app=='tail':
  flags=[['--workers',v] for v in ('1','4','64','0','bad','184467440737095516160')]+[['--workers'],['--workers=4']]
  for flag in flags:
   for path in (source,missing):
    cases += [flag+[str(path)],[str(path)]+flag]
  cases += [['--help','--workers','1'],['--workers','1','--help']]
 else:
  flags=[['--buffer-size','67108865'],['--pipeline-depth','65'],['--buffer-size','67108864','--pipeline-depth','9'],['--buffer-size','184467440737095516160']]
  for flag in flags:
   for atomic in ([],['--no-atomic']):
    for path in (source,missing):cases += [flag+atomic+[str(path),str(dest)],[str(path),str(dest)]+flag+atomic]
 for index,case in enumerate(cases):
  dest.write_text('sentinel\n')
  trace=args.output/f'{index}.strace';cmd=['strace','-f','-e','trace=open,openat,openat2,creat','-o',str(trace),str(binary),*case]
  r=subprocess.run(cmd,capture_output=True,timeout=10);(args.output/f'{index}.stdout').write_bytes(r.stdout);(args.output/f'{index}.stderr').write_bytes(r.stderr)
  diagnostic='--workers has been removed; omit it to use the app-owned driver' if args.app=='tail' else 'usage:'
  opened=any('"'+str(path)+'"' in trace.read_text() for path in (source,missing,dest))
  ok=r.returncode==1 and not r.stdout and diagnostic.encode() in r.stderr and not opened and dest.read_text()=='sentinel\n' and set(p.name for p in d.iterdir())=={'source','destination'}
  records.append(dict(command=cmd,exit_code=r.returncode,opened_target=opened,status='PASS' if ok else 'FAIL'))
  if not ok:failures.append(index)
 if args.app=='tail':
  r=subprocess.run([str(binary),'--help'],capture_output=True,timeout=10);(args.output/'help.stderr').write_bytes(r.stderr)
  if r.returncode!=0 or b'--workers' in r.stderr:failures.append('help')
(args.output/'results.json').write_text(json.dumps(dict(app=args.app,records=records,failures=failures),indent=2)+'\n');print(args.app,'cases',len(records),'failures',failures);raise SystemExit(bool(failures))
