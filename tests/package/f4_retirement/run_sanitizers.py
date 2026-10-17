"""Isolated Clang sanitizer package witnesses; no system dependency installation.

SUPERSEDED by the v2 root: part of the archived pre-v2 F4 campaign. Its
probes/retained.cpp input was removed with PR B; the campaign record and
inputs survive in Git history and docs/evidence/f4-retirement-20261012."""
import importlib.util
import json
from pathlib import Path
import sys

REPO=Path(__file__).resolve().parents[3]
spec=importlib.util.spec_from_file_location('f4',REPO/'scripts/verify_f4_retirement.py')
f4=importlib.util.module_from_spec(spec);spec.loader.exec_module(f4)
root=Path(sys.argv[1]).resolve()
ex=f4.Experiment(root)
source=Path(sys.argv[2]).resolve()
sha=f4.git(ex,source,'rev-parse','HEAD')
results=[]
for mode,flags in [('asanubsan',['-fsanitize=address,undefined','-fno-omit-frame-pointer']),('tsan',['-fsanitize=thread','-fno-omit-frame-pointer'])]:
 for profile in ('no-liburing','real-liburing'):
  key=mode+'-'+profile;tree=root/'worktrees'/key;tree.parent.mkdir(exist_ok=True)
  row=dict(mode=mode,profile=profile,source_sha=sha)
  try:
   ex.checked(['git','-C',source,'worktree','add','--detach',tree,sha])
   build=root/'build'/key;build.mkdir(parents=True)
   prefix=root/'install'/key;prefix.mkdir(parents=True)
   real=profile=='real-liburing'
   ex.checked(['xmake','f','-c','-y','--toolchain=clang','-m',mode,'-o',build/'native',f'--liburing={"y" if real else "n"}'],cwd=tree)
   for target in ('sluice_core','sluice_async'):
    ex.checked(['xmake','-y','-j','4',target],cwd=tree,timeout=600)
    ex.checked(['xmake','install','-o',prefix,target],cwd=tree)
   row['consumers']={}
   for name in ('w01_direct','w02_pipeline','w03_external_loop','retained'):
    src=build/(name+'.cpp')
    original=(f4.PROBES/'retained.cpp') if name=='retained' else REPO/'tests/package/consumers'/(name+'.cpp')
    src.write_bytes(original.read_bytes());binary=build/name
    c,_,_=ex.run(['clang++','-std=c++20','-pthread',*flags,f'-I{prefix}/include',*(['-DSLUICE_HAS_LIBURING'] if real else []),src,f'-L{prefix}/lib',*(['-lsluice_core'] if name=='w01_direct' else ['-lsluice_async','-lsluice_core']),*(['-luring'] if real else []),'-o',binary])
    probe=dict(build=c['id'],build_status=f4.status(c))
    if c['exit_code']==0:
     for variant in (['threadpool','uring'] if name=='w03_external_loop' else ['']):
      r,_,_=ex.run([binary,*([variant] if variant else [])],timeout=90)
      probe[variant or 'runtime']=dict(command=r['id'],status=f4.status(r,unavailable=name=='w03_external_loop'))
     loader,_,_=ex.run(['ldd',binary]);probe['loader']=loader['id']
    row['consumers'][name]=probe
   row['status']='PASS' if all(p.get('build_status')=='PASS' and p.get('runtime',p.get('threadpool',{})).get('status')=='PASS' for p in row['consumers'].values()) else 'FAIL'
  except Exception as e: row.update(status='UNAVAILABLE',reason=str(e))
  results.append(row)
  (root/'evidence.json').write_text(json.dumps(dict(source_sha=sha,results=results,commands=ex.commands),indent=2)+'\n')
  print(key,row['status'],flush=True)
sys.exit(any(r['status']!='PASS' for r in results))
