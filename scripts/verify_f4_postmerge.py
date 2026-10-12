#!/usr/bin/env python3
"""Fresh merged-master package execution; never relabel candidate evidence."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import tarfile
import sys
import os
import tempfile
import functools
import subprocess
import verify_f4_retirement as f4

ARCHIVE_SHA='d4f58f1230caa764c87300ebaf299617edf8adb54c8d0c33cd468f56272efa38'
CONSUMERS=('w01_direct','w02_pipeline','w03_external_loop','w04_stackful_candidate','contract_request_lifetime','contract_admission','contract_cancel_effect','contract_observer','contract_progress','contract_shutdown','contract_durability','contract_backend_availability')
REMOVED=('blocking_io_pool','buffer','copy','copy_strategy','fault','file','io_context','observed','reader','wal','writer')


@functools.lru_cache(maxsize=1)
def historical_reference():
    path=f4.REPO/'docs/evidence/f4-retirement-20261012'/(ARCHIVE_SHA+'.tar.gz')
    if f4.digest(path)!=ARCHIVE_SHA:raise RuntimeError('historical artifact checksum')
    with tarfile.open(path,'r:gz') as t:
        return json.load(t.extractfile('./run-02/evidence.json')), {r['path']:r['sha256'] for r in json.load(t.extractfile('./FILE_INDEX.json'))}


def source_names():
    return ['scripts/verify_f4_postmerge.py','scripts/verify_f4_retirement.py','tests/package/f4_retirement/surface_manifest.json','tests/package/f4_retirement/probes/retained.cpp',*['tests/package/consumers/'+n+'.cpp' for n in CONSUMERS]]


def verify(d,root):
    errors=[]
    def need(ok,name):
        if not ok: errors.append(name)
    old,index=historical_reference()
    need(d.get('producer_sha')==d['producer_identity']['sha'] and bool(f4.SHA.fullmatch(d.get('producer_sha',''))),'producer identity')
    need(d.get('historical_control')=={'source_sha':old['trees']['B0']['sha'],'archive_sha256':ARCHIVE_SHA},'historical control identity')
    need(set(d['sealed_inputs'])==set(source_names()),'complete sealed input set')
    canonical=json.loads(f4.MANIFEST.read_text())['surfaces']
    need(d['manifest']['surfaces']==canonical,'exact 17 manifest')
    need(d['master_execution_sha']==d['master_tested_sha'],'execution SHA')
    need(d['master_tree_sha']==d['identity']['tree'] and d['master_tested_sha']==d['identity']['sha'],'tree/source identity')
    need({r['path'] for r in d['removed_sources']}=={'src/'+n+'.cpp' for n in REMOVED},'exact removed source paths')
    for name,rec in d['sealed_inputs'].items():
        blob=subprocess.run(['git','-C',str(f4.REPO),'show',d['producer_identity']['sha']+':'+name],capture_output=True)
        need(blob.returncode==0 and hashlib.sha256(blob.stdout).hexdigest()==rec['sha256'],'committed producer content '+name)
        path=root/rec['file'];need(path.is_file() and f4.digest(path)==rec['sha256'],'sealed input '+name)
    cmds=d['commands']
    def log(cid,stream='stdout'): return (root/cmds[cid][stream]).read_text()
    def passed(cid): return isinstance(cid,int) and 0<=cid<len(cmds) and cmds[cid]['exit_code']==0
    need(d['kind']=='POSTMERGE_MASTER','identity')
    need(bool(f4.SHA.fullmatch(d['master_tested_sha'])),'full SHA')
    need(len(d['packages'])==4 and {(p['profile'],p['mode']) for p in d['packages']}=={(p,m) for p in ('no-liburing','real-liburing') for m in ('debug','release')},'four profiles')
    for c in cmds:
        for stream in ('stdout','stderr'):
            p=root/c[stream];need(p.is_file() and f4.digest(p)==c[stream+'_sha256'],'log hash '+str(c['id']))
        if Path(c['argv'][0]).name in ('g++','clang++'):
            need(all(c['environment'].get(k) is None for k in f4.SCRUB),'compiler environment')
            for i,arg in enumerate(c['argv']):
                for flag in ('-I','-isystem','-iquote','-include','-idirafter'):
                    if arg==flag: value=c['argv'][i+1] if i+1<len(c['argv']) else ''
                    elif arg.startswith(flag): value=arg[len(flag):]
                    else: continue
                    need(value in {p['prefix']+'/include' for p in d['packages']},'source include leak')
    for p in d['packages']:
        key=p['profile']+'/'+p['mode'];need(p['tree_id']=='MASTER' and p['source_sha']==d['master_tested_sha'] and p['input_hashes']==d['input_hashes'],'measured source '+key)
        required=set(CONSUMERS)
        need(set(p['consumers'])==required,'required consumer set '+key)
        macro=log(p['macro_view']);need(passed(p['macro_view']) and ('#define SLUICE_HAS_LIBURING ' in macro)==(p['profile']=='real-liburing'),'profile macro '+key)
        undefined=log(p['archives']['async']['undefined']);need(('io_uring' in undefined)==(p['profile']=='real-liburing'),'archive macro backend '+key)
        need(set(p['consumer_inputs'])==set(CONSUMERS),'complete consumer source set '+key)
        need({r['path'] for r in p['installed_files']}=={'include/'+h for h in p['headers']}|{'lib/libsluice_core.a','lib/libsluice_async.a'} and len(p['installed_files'])==58,'complete installed inventory '+key)
        control=next(r for r in old['packages'] if r['tree_id']=='C508510' and r['profile']==p['profile'] and r['mode']==p['mode'])
        need(p['retired_symbols']['expected_absent']==control['retired_symbols']['expected_absent'],'historical strong symbols '+key)
        for name,rec in p['consumer_inputs'].items():
            need(f4.digest(root/rec['file'])==rec['sha256'] and p['consumers'][name]['source_sha256']==rec['sha256'],'consumer source '+key+'/'+name)
        for rec in p['installed_files']:
            path=Path(p['prefix'])/rec['path'];need(path.is_file() and f4.digest(path)==rec['sha256'],'installed file '+key+'/'+rec['path'])
        counts=[]
        for lib,a in p['archives'].items():
            need(passed(a['defined']) and passed(a['undefined']) and passed(a['member_command']) and passed(a['sha_command']),'archive tools '+key+'/'+lib)
            need(f4.digest(Path(p['prefix'])/'lib'/('libsluice_'+lib+'.a'))==a['sha256'],'actual archive '+key+'/'+lib)
            counts.append(a['count'])
            raw=log(a['member_command']).strip().splitlines();need(raw==a['members'] and len(raw)==a['count'],'ar count '+key+'/'+lib)
            need(log(a['sha_command']).split()[0]==a['sha256'],'archive SHA '+key+'/'+lib)
        need(counts==[2,33] and p['counts']==[2,33,56] and len(p['headers'])==56,'inventory '+key)
        need(len(p['surfaces'])==17,'17 interfaces '+key)
        for s in d['manifest']['surfaces']:
            r=p['surfaces'][s['id']];c=cmds[r['include']];err=log(r['include'],'stderr')
            need(not r['present'] and r['negative_status']=='PASS' and c['exit_code'] not in (0,None) and s['header'] in err and ('No such file' in err or 'file not found' in err),'negative header '+key+'/'+s['id'])
            if s['link_negative']:
                r=p['negative_links'][s['id']];need(r['object_sha256']==index[f"run-02/build/B0/{p['profile']}/{p['mode']}/{s['id']}-api.o"] and r['baseline_archive_sha256']==index[f"run-02/install/B0/{p['profile']}/{p['mode']}/lib/libsluice_core.a"],'immutable historical relink inputs '+key+'/'+s['id']);need(f4.digest(root/r['object_file'])==r['object_sha256'] and f4.digest(root/r['baseline_archive'])==r['baseline_archive_sha256'],'historical relink inputs '+key+'/'+s['id']);need(cmds[r['baseline_link']]['argv'][2]==cmds[r['candidate_link']]['argv'][2]==str(root/r['object_file']),'same relink object '+key+'/'+s['id']);need(any(symbol in log(r['candidate_link'],'stderr') for symbol in p['retired_symbols']['expected_absent']),'specific retired undefined symbol '+key+'/'+s['id']);need(passed(r['baseline_link']) and cmds[r['candidate_link']]['exit_code'] not in (0,None) and 'undefined reference' in log(r['candidate_link'],'stderr'),'ABI relink '+key+'/'+s['id'])
        need(p['retired_symbols']['expected_absent']==d['historical_retired_symbols'],'exact retired symbols '+key)
        if p['profile']=='real-liburing' and p['mode']=='debug':
            need(set(p.get('progress_traces',{}))=={'threadpool','uring'},'progress traces')
            for backend,r in p.get('progress_traces',{}).items():
                path=root/r['file'];trace=path.read_text();need(passed(r['command']) and f4.digest(path)==r['sha256'] and 'epoll_wait(' in trace and re.search(r'epoll_wait\([^\n]*, [1-9][0-9]*\)',trace),'blocking epoll trace '+backend)
        defined='\n'.join(log(p['archives'][lib]['defined']) for lib in ('core','async'))
        need(p['retired_symbols']['expected_absent'] and not(set(p['retired_symbols']['expected_absent']) & f4.symbol_names(defined)),'retired callable '+key)
        need(passed(p['public_closure']['command']),'public closure '+key)
        need(all(passed(p['retained'][x]) for x in ('compile','link','run')),'retained '+key)
        for name,row in p['consumers'].items():
            need(passed(row['compile_link']),'consumer link '+key+'/'+name)
            if name=='w03_external_loop':
                need(passed(row['threadpool']['command']) and row['no_scheduler_fiber']=='PASS','W03 threadpool/firewall '+key)
                rc=cmds[row['uring']['command']]['exit_code'];expected=0 if p['profile']=='real-liburing' else 2
                need(rc==expected,'W03 uring availability '+key)
            elif name!='w04_stackful_candidate': need(passed(row['runtime']['command']),'consumer run '+key+'/'+name)
    need(len(d['removed_sources'])==11 and all(not r['present'] for r in d['removed_sources']),'11 source retirements')
    return errors


def seal(root):
    d=json.loads((root/'evidence.json').read_text())
    tree=Path(d.get('source_path',str(f4.REPO.parent/'master-source')))
    sha=subprocess_capture(['git','-C',str(tree),'rev-parse','HEAD'])
    if sha!=d['master_tested_sha']: raise RuntimeError('master source changed')
    d['producer_sha']=d.get('producer_sha','8999d180872dbc7465b669d7dc8d88958f0b383c')
    d['producer_identity']={'sha':d['producer_sha']}
    d['identity']={'sha':sha,'tree':subprocess_capture(['git','-C',str(tree),'rev-parse','HEAD^{tree}'])}
    d['sealed_inputs']={}
    snapshot=Path(tempfile.mkdtemp(prefix='executed-inputs-',dir=root))
    files=['scripts/verify_f4_postmerge.py','scripts/verify_f4_retirement.py','tests/package/f4_retirement/surface_manifest.json']
    files += ['tests/package/f4_retirement/probes/retained.cpp']
    files += ['tests/package/consumers/'+n+'.cpp' for n in CONSUMERS]
    import subprocess
    for name in files:
        blob=subprocess.run(['git','-C',str(f4.REPO),'show',d.get('producer_sha','8999d180872dbc7465b669d7dc8d88958f0b383c')+':'+name],check=True,capture_output=True).stdout
        path=snapshot/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(blob)
        d['sealed_inputs'][name]={'file':str(path.relative_to(root)),'sha256':f4.digest(path)}
    d['historical_retired_symbols']=d['packages'][0]['retired_symbols']['expected_absent']
    for p in d['packages']:
        p['installed_files']=[{'path':str(f.relative_to(p['prefix'])),'sha256':f4.digest(f)} for f in sorted(Path(p['prefix']).rglob('*')) if f.is_file()]
        p['consumer_inputs']={n:d['sealed_inputs']['tests/package/consumers/'+n+'.cpp'] for n in p['consumers']}
        for sid,r in p['negative_links'].items():
            obj=Path(d['commands'][r['baseline_link']]['argv'][2]);lib=Path(d['commands'][r['baseline_link']]['argv'][3])
            r.update(object_file=str(obj.relative_to(root)),baseline_archive=str(lib.relative_to(root)),baseline_archive_sha256=f4.digest(lib))
        for backend,r in p.get('progress_traces',{}).items():r['file']=backend+'.strace'
    d['seal_note']='Metadata sealing of actual 8999d180 execution; no compilation/runtime repeated or relabeled'
    (root/'evidence-sealed.json').write_text(json.dumps(d,indent=2)+'\n')
    return d


def subprocess_capture(cmd):
    import subprocess
    return subprocess.run(cmd,check=True,capture_output=True,text=True).stdout.strip()


def main():
    ap=argparse.ArgumentParser();ap.add_argument('--source',type=Path);ap.add_argument('--master-sha');ap.add_argument('--output',type=Path);ap.add_argument('--verify-evidence',type=Path);ap.add_argument('--seal-existing',type=Path);args=ap.parse_args()
    if args.seal_existing:
        d=seal(args.seal_existing);errors=verify(d,args.seal_existing);print(json.dumps({'mode':'METADATA_SEALING_ONLY','errors':errors}));return bool(errors)
    if args.verify_evidence:
        sealed=args.verify_evidence/'evidence-sealed.json';d=json.loads(sealed.read_text() if sealed.exists() else (args.verify_evidence/'evidence.json').read_text());errors=verify(d,args.verify_evidence);print(json.dumps({'mode':'INTEGRITY_ONLY','errors':errors}));return bool(errors)
    if not all((args.source,args.master_sha,args.output)):ap.error('source, master-sha, output required')
    ex=f4.Experiment(args.output);source=args.source.resolve();sha=f4.git(ex,source,'rev-parse','HEAD')
    if sha!=args.master_sha or f4.git(ex,source,'status','--porcelain=v1'):raise RuntimeError('master source identity/cleanliness mismatch')
    manifest=json.loads(f4.MANIFEST.read_text());manifest['expected']['MASTER']=[2,33,56]
    d=dict(kind='POSTMERGE_MASTER',producer_sha=f4.git(ex,f4.REPO,'rev-parse','HEAD'),source_path=str(source),master_execution_sha=sha,master_tested_sha=sha,master_tree_sha=f4.git(ex,source,'rev-parse','HEAD^{tree}'),input_hashes=f4.input_hashes(ex,source),manifest=manifest,packages=[],commands=ex.commands,external_use='UNKNOWN',removed_sources=[dict(path='src/'+n+'.cpp',present=(source/'src'/(n+'.cpp')).exists()) for n in REMOVED],optional_w04_supported=False)
    archive=f4.REPO/'docs/evidence/f4-retirement-20261012'/(ARCHIVE_SHA+'.tar.gz')
    if f4.digest(archive)!=ARCHIVE_SHA:raise RuntimeError('historical artifact digest mismatch')
    control=ex.output/'historical-control';control.mkdir()
    with tarfile.open(archive,'r:gz') as t:
        old=json.load(t.extractfile('./run-02/evidence.json'));d['historical_control']=dict(source_sha=old['trees']['B0']['sha'],archive_sha256=ARCHIVE_SHA)
        expected=next(p['retired_symbols']['expected_absent'] for p in old['packages'] if p['tree_id']=='C508510')
        for profile in ('no-liburing','real-liburing'):
            for mode in ('debug','release'):
                p=f4.package(ex,source,'MASTER',profile,mode,manifest,retire_all=True);d['packages'].append(p)
                p['retired_symbols']=dict(expected_absent=expected)
                p['negative_links']={};base=control/profile/mode;base.mkdir(parents=True)
                lib=base/'libsluice_core.a';lib.write_bytes(t.extractfile(f'./run-02/install/B0/{profile}/{mode}/lib/libsluice_core.a').read())
                for s in manifest['surfaces']:
                    if not s['link_negative']:continue
                    obj=base/(s['id']+'.o');obj.write_bytes(t.extractfile(f'./run-02/build/B0/{profile}/{mode}/{s["id"]}-api.o').read())
                    b,_,_=ex.run(['g++','-pthread',obj,lib,'-o',base/(s['id']+'-positive')])
                    n,_,_=ex.run(['g++','-pthread',obj,f'-L{p["prefix"]}/lib','-lsluice_core','-o',base/(s['id']+'-negative')])
                    p['negative_links'][s['id']]=dict(baseline_link=b['id'],candidate_link=n['id'],object_sha256=f4.digest(obj))
                if profile=='real-liburing' and mode=='debug':
                    for backend in ('threadpool','uring'):
                        binary=ex.output/'build/MASTER'/profile/mode/'w03_external_loop';trace=ex.output/(backend+'.strace')
                        c,_,_=ex.run(['strace','-f','-e','trace=epoll_wait,epoll_ctl,eventfd2','-o',trace,binary,backend]);p.setdefault('progress_traces',{})[backend]=dict(command=c['id'],sha256=f4.digest(trace))
    for argv in (['g++','--version'],['clang++','--version'],['xmake','--version'],['uname','-a'],['pkg-config','--modversion','liburing']):ex.run(argv)
    (ex.output/'evidence.json').write_text(json.dumps(d,indent=2)+'\n');d=seal(ex.output);errors=verify(d,ex.output)
    (ex.output/'verification.json').write_text(json.dumps({'errors':errors,'status':'FAIL' if errors else 'PASS'},indent=2)+'\n')
    (ex.output/'REPRODUCE.md').write_text(shlex.join(sys.argv)+'\n\n'+'\n'.join(f"{c['id']} rc={c['exit_code']} cwd={c['cwd']} {shlex.join(c['argv'])}" for c in ex.commands)+'\n')
    print(json.dumps({'master_tested_sha':sha,'errors':errors}));return bool(errors)


if __name__=='__main__':sys.exit(main())
