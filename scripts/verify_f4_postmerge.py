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
import verify_f4_retirement as f4

ARCHIVE_SHA='d4f58f1230caa764c87300ebaf299617edf8adb54c8d0c33cd468f56272efa38'
REMOVED=('blocking_io_pool','buffer','copy','copy_strategy','fault','file','io_context','observed','reader','wal','writer')


def verify(d,root):
    errors=[]
    def need(ok,name):
        if not ok: errors.append(name)
    cmds=d['commands']
    def log(cid,stream='stdout'): return (root/cmds[cid][stream]).read_text()
    def passed(cid): return isinstance(cid,int) and 0<=cid<len(cmds) and cmds[cid]['exit_code']==0
    need(d['kind']=='POSTMERGE_MASTER','identity')
    need(bool(f4.SHA.fullmatch(d['master_tested_sha'])),'full SHA')
    need(len(d['packages'])==4 and {(p['profile'],p['mode']) for p in d['packages']}=={(p,m) for p in ('no-liburing','real-liburing') for m in ('debug','release')},'four profiles')
    for c in cmds:
        for stream in ('stdout','stderr'):
            p=root/c[stream];need(p.is_file() and f4.digest(p)==c[stream+'_sha256'],'log hash '+str(c['id']))
        if Path(c['argv'][0]).name=='g++':
            for arg in c['argv']:
                if arg.startswith('-I'):
                    need(arg[2:] in {p['prefix']+'/include' for p in d['packages']},'source include leak')
    for p in d['packages']:
        key=p['profile']+'/'+p['mode'];need(p['tree_id']=='MASTER' and p['source_sha']==d['master_tested_sha'] and p['input_hashes']==d['input_hashes'],'measured source '+key)
        counts=[]
        for lib,a in p['archives'].items():
            raw=log(a['member_command']).strip().splitlines();need(raw==a['members'] and len(raw)==a['count'],'ar count '+key+'/'+lib)
            need(log(a['sha_command']).split()[0]==a['sha256'],'archive SHA '+key+'/'+lib)
        need(p['counts']==[2,33,56] and len(p['headers'])==56,'inventory '+key)
        need(len(p['surfaces'])==17,'17 interfaces '+key)
        for s in d['manifest']['surfaces']:
            r=p['surfaces'][s['id']];c=cmds[r['include']];err=log(r['include'],'stderr')
            need(not r['present'] and r['negative_status']=='PASS' and c['exit_code'] not in (0,None) and s['header'] in err and ('No such file' in err or 'file not found' in err),'negative header '+key+'/'+s['id'])
            if s['link_negative']:
                r=p['negative_links'][s['id']];need(passed(r['baseline_link']) and cmds[r['candidate_link']]['exit_code'] not in (0,None) and 'undefined reference' in log(r['candidate_link'],'stderr'),'ABI relink '+key+'/'+s['id'])
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


def main():
    ap=argparse.ArgumentParser();ap.add_argument('--source',type=Path);ap.add_argument('--master-sha');ap.add_argument('--output',type=Path);ap.add_argument('--verify-evidence',type=Path);args=ap.parse_args()
    if args.verify_evidence:
        d=json.loads((args.verify_evidence/'evidence.json').read_text());errors=verify(d,args.verify_evidence);print(json.dumps({'mode':'INTEGRITY_ONLY','errors':errors}));return bool(errors)
    if not all((args.source,args.master_sha,args.output)):ap.error('source, master-sha, output required')
    ex=f4.Experiment(args.output);source=args.source.resolve();sha=f4.git(ex,source,'rev-parse','HEAD')
    if sha!=args.master_sha or f4.git(ex,source,'status','--porcelain=v1'):raise RuntimeError('master source identity/cleanliness mismatch')
    manifest=json.loads(f4.MANIFEST.read_text());manifest['expected']['MASTER']=[2,33,56]
    d=dict(kind='POSTMERGE_MASTER',master_execution_sha=sha,master_tested_sha=sha,master_tree_sha=f4.git(ex,source,'rev-parse','HEAD^{tree}'),input_hashes=f4.input_hashes(ex,source),manifest=manifest,packages=[],commands=ex.commands,external_use='UNKNOWN',removed_sources=[dict(path='src/'+n+'.cpp',present=(source/'src'/(n+'.cpp')).exists()) for n in REMOVED],optional_w04_supported=False)
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
    (ex.output/'evidence.json').write_text(json.dumps(d,indent=2)+'\n');errors=verify(d,ex.output)
    (ex.output/'verification.json').write_text(json.dumps({'errors':errors,'status':'FAIL' if errors else 'PASS'},indent=2)+'\n')
    (ex.output/'REPRODUCE.md').write_text(shlex.join(sys.argv)+'\n\n'+'\n'.join(f"{c['id']} rc={c['exit_code']} cwd={c['cwd']} {shlex.join(c['argv'])}" for c in ex.commands)+'\n')
    print(json.dumps({'master_tested_sha':sha,'errors':errors}));return bool(errors)


if __name__=='__main__':sys.exit(main())
