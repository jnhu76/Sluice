#!/usr/bin/env python3
"""Versioned F4 package experiment. Evidence verification never reruns commands.

SUPERSEDED by the v2 root (docs/explicit-io-contract.md): this tool and its
recorded evidence document the pre-v2 F4 surface-retirement campaign. Its
consumer inputs (w04_stackful_candidate.cpp, probes/retained.cpp) were
removed from the live tree with PR B and survive only in Git history and the
archived campaign tarball; it is dated evidence, not current authority. The
active package verifier is scripts/verify_v2_package.py."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys

REPO = Path(__file__).resolve().parents[1]
MANIFEST = REPO / 'tests/package/f4_retirement/surface_manifest.json'
PROBES = MANIFEST.parent / 'probes'
SHA = re.compile(r'^[0-9a-f]{40}$')
SCRUB = ('CPATH', 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH', 'OBJC_INCLUDE_PATH',
         'LIBRARY_PATH', 'CXXFLAGS', 'CFLAGS', 'CPPFLAGS', 'LDFLAGS', 'LD_PRELOAD')


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def clean_env():
    return {k: v for k, v in os.environ.items() if k not in SCRUB}


class Experiment:
    def __init__(self, output):
        self.output = output.resolve()
        self.output.mkdir(parents=True, exist_ok=False)
        self.logs = self.output / 'logs'
        self.logs.mkdir()
        self.commands = []

    def run(self, argv, cwd=None, env=None, timeout=300):
        argv = list(map(str, argv))
        n = len(self.commands)
        out = self.logs / f'{n:05}.stdout'
        err = self.logs / f'{n:05}.stderr'
        try:
            r = subprocess.run(argv, cwd=cwd, env=env or clean_env(),
                               capture_output=True, timeout=timeout)
            rc, stdout, stderr = r.returncode, r.stdout, r.stderr
        except (OSError, subprocess.TimeoutExpired) as e:
            rc, stdout, stderr = None, getattr(e, 'stdout', None) or b'', str(e).encode()
        out.write_bytes(stdout)
        err.write_bytes(stderr)
        record = dict(id=n, argv=argv, cwd=str(cwd or Path.cwd()), exit_code=rc,
                      stdout=str(out.relative_to(self.output)), stderr=str(err.relative_to(self.output)),
                      stdout_sha256=digest(out), stderr_sha256=digest(err),
                      environment={k: (env or clean_env()).get(k) for k in (*SCRUB, 'LD_LIBRARY_PATH')})
        self.commands.append(record)
        return record, stdout.decode(errors='replace'), stderr.decode(errors='replace')

    def checked(self, argv, **kw):
        c, out, err = self.run(argv, **kw)
        if c['exit_code'] != 0:
            raise RuntimeError(f"command {c['id']} BLOCKED: {shlex.join(c['argv'])}\n{err[-2000:]}")
        return out.strip()


def git(ex, tree, *args):
    return ex.checked(['git', '-C', tree, *args])


def input_hashes(ex, tree):
    result = {}
    for label, paths in [('production', ['include', 'src']), ('tests', ['tests']),
                         ('build', ['xmake.lua', 'xmake'])]:
        names = git(ex, tree, 'ls-files', '--', *paths).splitlines()
        h = hashlib.sha256()
        for name in names:
            h.update(name.encode() + b'\0' + (tree / name).read_bytes() + b'\0')
        result[label] = h.hexdigest()
    return result


def status(command, unavailable=False):
    rc = command['exit_code']
    return 'PASS' if rc == 0 else 'UNAVAILABLE' if unavailable and rc == 2 else 'BLOCKED' if rc is None else 'FAIL'


def symbol_names(text):
    return {line.split(None, 2)[2] for line in text.splitlines()
            if re.match(r'^[0-9a-fA-F]+ [A-Za-z] ', line)}


def compile_probe(ex, prefix, scratch, name, code, flags, links):
    source = scratch / f'{name}.cpp'
    source.write_text(code)
    obj = scratch / f'{name}.o'
    binary = scratch / name
    c, _, _ = ex.run(['g++', '-std=c++20', '-O0', '-pthread', f'-I{prefix}/include',
                     *flags, '-c', source, '-o', obj], cwd=scratch)
    result = dict(compile=c['id'], compile_status=status(c))
    if c['exit_code'] == 0:
        l, _, _ = ex.run(['g++', '-pthread', obj, f'-L{prefix}/lib', *links, '-o', binary], cwd=scratch)
        result.update(link=l['id'], link_status=status(l))
        if l['exit_code'] == 0:
            r, _, _ = ex.run([binary], cwd=scratch, timeout=60)
            result.update(run=r['id'], run_status=status(r))
    return result


def package(ex, tree, tree_id, profile, mode, manifest, retire_all=False):
    key = f'{tree_id}/{profile}/{mode}'
    scratch = ex.output / 'build' / key
    prefix = ex.output / 'install' / key
    scratch.mkdir(parents=True)
    prefix.mkdir(parents=True)
    real = profile == 'real-liburing'
    flags = ['-DSLUICE_HAS_LIBURING'] if real else []
    links = ['-lsluice_async', '-lsluice_core', *(['-luring'] if real else [])]
    env = clean_env()
    print(f'BUILD {key}', flush=True)
    ex.checked(['xmake', 'f', '-c', '-y', '-m', mode, '-o', scratch / 'native',
                f'--liburing={"y" if real else "n"}'], cwd=tree, env=env)
    for target in ('sluice_core', 'sluice_async'):
        ex.checked(['xmake', '-y', '-j', '4', target], cwd=tree, env=env, timeout=600)
        ex.checked(['xmake', 'install', '-o', prefix, target], cwd=tree, env=env)
    result = dict(tree_id=tree_id, profile=profile, mode=mode, prefix=str(prefix),
                  source_sha=git(ex, tree, 'rev-parse', 'HEAD'), input_hashes=input_hashes(ex, tree),
                  headers=sorted(str(p.relative_to(prefix / 'include')) for p in (prefix / 'include').rglob('*.hpp')),
                  archives={}, surfaces={}, consumers={})
    for lib in ('core', 'async'):
        path = prefix / 'lib' / f'libsluice_{lib}.a'
        member_cmd, member_out, _ = ex.run(['ar', 't', path])
        members = member_out.strip().splitlines()
        c, defined, _ = ex.run(['nm', '-C', '--defined-only', path])
        u, _, _ = ex.run(['nm', '-C', '--undefined-only', path])
        raw, _, _ = ex.run(['nm', '--defined-only', path])
        sha_cmd, _, _ = ex.run(['sha256sum', path])
        result['archives'][lib] = dict(members=members, count=len(members), sha256=digest(path),
                                      member_command=member_cmd['id'], defined=c['id'], undefined=u['id'], raw_defined=raw['id'], sha_command=sha_cmd['id'])
    result['counts'] = [result['archives']['core']['count'], result['archives']['async']['count'], len(result['headers'])]
    result['inventory_status'] = 'PASS' if result['counts'] == manifest['expected'][tree_id] else 'FAIL'
    macro, _, _ = ex.run(['g++', '-std=c++20', *flags, '-dM', '-E', '-x', 'c++', '/dev/null'])
    result['macro_view'] = macro['id']
    for s in manifest['surfaces']:
        h = s['header']
        removed = retire_all or tree_id == 'C508510' or tree_id == f"C{s['group']}"
        code = f'#include <{h}>\nint main(){{}}\n'
        src = scratch / (s['id'] + '.cpp')
        src.write_text(code)
        cmd, _, err = ex.run(['g++', '-std=c++20', '-H', f'-I{prefix}/include', *flags,
                             '-fsyntax-only', src], cwd=scratch)
        row = dict(header=h, present=h in result['headers'], include=cmd['id'], removed=removed,
                   standalone=status(cmd))
        if removed:
            row['negative_status'] = ('PASS' if not row['present'] and cmd['exit_code'] not in (None, 0)
                                      and h in err and ('No such file' in err or 'file not found' in err) else 'FAIL')
        elif s['parent']:
            src.write_text(f'#include <{s["parent"]}>\n#include <{h}>\nint main(){{}}\n')
            parent, _, _ = ex.run(['g++', '-std=c++20', '-H', f'-I{prefix}/include', *flags, '-fsyntax-only', src])
            row.update(parent_include=parent['id'], parent_status=status(parent))
        if not removed and s['probe']:
            probe_code = (PROBES / (s['probe'] + '.cpp')).read_text()
            row['api'] = compile_probe(ex, prefix, scratch, s['id'] + '-api', probe_code, flags, ['-lsluice_core'])
        result['surfaces'][s['id']] = row
    all_headers = scratch / 'public-closure.cpp'
    all_headers.write_text(''.join(f'#include <{h}>\n' for h in result['headers'] if not ('_impl.hpp' in h or '/detail/' in h))+'int main(){}\n')
    closure, _, _ = ex.run(['g++','-std=c++20','-H',f'-I{prefix}/include',*flags,'-fsyntax-only',all_headers])
    result['public_closure'] = dict(command=closure['id'],status=status(closure))
    result['retained'] = compile_probe(ex, prefix, scratch, 'retained', (PROBES / 'retained.cpp').read_text(), flags, links)
    consumers = REPO / 'tests/package/consumers'
    for name in ['w01_direct', 'w02_pipeline', 'w03_external_loop', 'w04_stackful_candidate',
                 'contract_request_lifetime', 'contract_admission', 'contract_cancel_effect',
                 'contract_observer', 'contract_progress', 'contract_shutdown', 'contract_durability',
                 'contract_backend_availability']:
        target = scratch / name
        source = scratch / (name + '.cpp')
        source.write_bytes((consumers / (name + '.cpp')).read_bytes())
        c, _, _ = ex.run(['g++', '-std=c++20', '-pthread', f'-I{prefix}/include', *flags,
                         source, f'-L{prefix}/lib', *(['-lsluice_core'] if name == 'w01_direct' else links),
                         '-Wl,-Map=' + str(target) + '.map', '-o', target], cwd=scratch)
        row = dict(compile_link=c['id'], compile_link_status=status(c), source_sha256=digest(source),source_origin=str(consumers/(name+'.cpp')))
        if c['exit_code'] == 0:
            variants = ['threadpool', 'uring'] if name == 'w03_external_loop' else ['']
            for variant in variants:
                r, out, _ = ex.run([target, *([variant] if variant else [])], cwd=scratch, timeout=90)
                row[variant or 'runtime'] = dict(command=r['id'], status=status(r, unavailable=name in ('w03_external_loop', 'w04_stackful_candidate')))
                if name=='w03_external_loop' and variant=='threadpool':
                    row['non_busy_progress']='PASS' if r['exit_code']==0 else status(r)
            n, nm, _ = ex.run(['nm', '-C', target])
            row['binary_nm'] = n['id']
            if name in ('w01_direct', 'w03_external_loop'):
                row['no_scheduler_fiber'] = 'PASS' if not re.search(r'sluice::async::(?:Scheduler|Fiber)::', nm) else 'FAIL'
        result['consumers'][name] = row
    return result


def verify(data, root):
    errors = []
    def require(ok, message):
        if not ok:
            errors.append(message)
    require(data.get('schema') == 1, 'schema')
    surfaces = data.get('manifest', {}).get('surfaces', [])
    require(len(surfaces) == 17 and len({s.get('header') for s in surfaces}) == 17, '17 unique surfaces required')
    commands = data.get('commands', [])
    for i, c in enumerate(commands):
        require(c.get('id') == i, f'command id {i}')
        for stream in ('stdout', 'stderr'):
            p = root / c.get(stream, '<missing>')
            require(p.is_file() and digest(p) == c.get(stream + '_sha256'), f'log hash {i} {stream}')
        argv = c.get('argv', [])
        if argv and Path(argv[0]).name in ('g++', 'clang++'):
            for j, arg in enumerate(argv):
                if arg.startswith(('-I', '-isystem', '-iquote', '-include')):
                    value = arg[2:] if arg.startswith('-I') else arg.split('=', 1)[-1]
                    if arg in ('-I', '-isystem', '-iquote', '-include'):
                        value = argv[j+1] if j+1 < len(argv) else ''
                    historical_ids={row.get(k) for row in data.get('historical', {}).values() for k in ('standalone','parent_include')}
                    installed_includes={str(Path(p['prefix'])/'include') for p in data.get('packages',[])}
                    require(value in installed_includes or (i in historical_ids and value == data['trees']['H0']['path']+'/include'), f'source include leakage {i}')
            require(all(c.get('environment', {}).get(k) is None for k in SCRUB), f'unscrubbed environment {i}')
    def passed(cid):
        return isinstance(cid,int) and 0 <= cid < len(commands) and commands[cid]['exit_code']==0
    for tid, tree in data.get('trees', {}).items():
        require(bool(SHA.fullmatch(tree.get('sha', ''))), f'SHA {tid}')
        require(bool(SHA.fullmatch(tree.get('tree', ''))), f'tree SHA {tid}')
        require(tree.get('sha') == data.get('inputs', {}).get(tid), f'input/measurement mismatch {tid}')
    require(set(data.get('trees', {})) == {'B0','H0','C508','C510','C508510'}, 'five trees required')
    packages = data.get('packages', [])
    require({(p.get('tree_id'),p.get('profile'),p.get('mode')) for p in packages} == {(t,p,m) for t in ('B0','C508','C510','C508510') for p in ('no-liburing','real-liburing') for m in ('debug','release')}, 'unique complete matrix')
    require(len(packages) == 16, 'full 4-tree 2-profile Debug/Release matrix required')
    for p in packages:
        key = f"{p['tree_id']}/{p['profile']}/{p['mode']}"
        require(p.get('source_sha') == data['trees'][p['tree_id']]['sha'], f'source SHA {key}')
        require(p.get('input_hashes') == data['trees'][p['tree_id']]['input_hashes'], f'input hashes {key}')
        require(len(p.get('surfaces', {})) == 17, f'missing surface {key}')
        counts = [len(p['archives']['core']['members']),len(p['archives']['async']['members']),len(p['headers'])]
        require(p.get('counts') == counts == data['manifest']['expected'][p['tree_id']], f'archive/header counts {key}')
        for lib, a in p['archives'].items():
            path = Path(p['prefix']) / 'lib' / f'libsluice_{lib}.a'
            if path.exists():
                require(digest(path) == a['sha256'], f'archive hash {key}/{lib}')
        require(all(p.get('retained', {}).get(x+'_status') == 'PASS' and passed(p.get('retained',{}).get(x)) for x in ('compile','link','run')), f'retained Future/WaitPolicy {key}')
        for lib, a in p['archives'].items():
            cid=a.get('member_command', -1)
            if 0 <= cid < len(commands):
                recorded=(root/commands[cid]['stdout']).read_text().strip().splitlines()
                require(a['members'] == recorded and a['count'] == len(recorded), f'ar raw member mismatch {key}/{lib}')
            else: require(False, f'missing ar command {key}/{lib}')
        for s in surfaces:
            row = p.get('surfaces', {}).get(s['id'], {})
            if row.get('removed'):
                cid = row.get('include', -1)
                c = commands[cid] if isinstance(cid, int) and 0 <= cid < len(commands) else {}
                diagnostic=(root/c['stderr']).read_text() if c.get('stderr') else ''
                require(row.get('negative_status') == 'PASS' and row.get('present') is False
                        and c.get('exit_code') not in (None, 0) and s['header'] in diagnostic and ('No such file' in diagnostic or 'file not found' in diagnostic), f'negative include {key}/{s["id"]}')
            else:
                require(row.get('present') and (row.get('parent_status') == 'PASS' if s['parent'] else row.get('standalone') == 'PASS'), f'baseline include {key}/{s["id"]}')
                if s['probe']:
                    require(all(row.get('api', {}).get(x+'_status') == 'PASS' and passed(row.get('api',{}).get(x)) for x in ('compile','link','run')), f'old API {key}/{s["id"]}')
        for name in ('w01_direct', 'w02_pipeline', 'w03_external_loop'):
            row = p['consumers'].get(name,{})
            require(row.get('compile_link_status') == 'PASS' and passed(row.get('compile_link')), f'consumer build {key}/{name}')
            r = row.get('threadpool' if name=='w03_external_loop' else 'runtime',{})
            require(r.get('status') == 'PASS' and passed(r.get('command')), f'consumer runtime {key}/{name}')
            if name == 'w03_external_loop':
                ur = row.get('uring', {})
                cid = ur.get('command', -1)
                rc = commands[cid]['exit_code'] if 0 <= cid < len(commands) else None
                require(ur.get('status') == ('PASS' if rc==0 else 'UNAVAILABLE' if rc==2 else 'FAIL'), f'uring unavailable mislabeled {key}')
                require(row.get('no_scheduler_fiber') == 'PASS', f'W03 dependency {key}')
        for name,row in p.get('consumers',{}).items():
            if name.startswith('contract_'):
                require(row.get('compile_link_status')=='PASS' and passed(row.get('compile_link')) and row.get('runtime',{}).get('status')=='PASS' and passed(row.get('runtime',{}).get('command')), f'contract runtime {key}/{name}')
        audit = p.get('retired_symbols', {})
        if p['tree_id'] != 'B0':
            defined='\n'.join((root/commands[p['archives'][lib]['defined']]['stdout']).read_text() for lib in ('core','async'))
            actual_remaining=sorted(set(audit.get('expected_absent', [])) & symbol_names(defined))
            require(audit.get('status') == 'PASS' and audit.get('expected_absent') and not actual_remaining and audit.get('remaining') == actual_remaining, f'retired callable symbols {key}')
            for sid, link in p.get('negative_links', {}).items():
                cid=link['command']
                require(link['status'] == 'PASS' and commands[cid]['exit_code'] not in (None,0), f'negative link {key}/{sid}')
    required_risk=('SURFACE_ID HEADER V001_PUBLIC_FACT CURRENT_INSTALLED_FACT CURRENT_IN_TREE_CONSUMER EXTERNAL_USE OLD_API_COMPILE OLD_API_LINK OLD_BEHAVIOR_WITNESS CANDIDATE_HEADER_ABSENT CANDIDATE_API_COMPILE_REJECTED CANDIDATE_SYMBOL_ABSENT RETAINED_CANONICAL_WORKLOAD MIGRATION_OR_NO_EQUIVALENT BREAKAGE_CONSEQUENCE PROPOSED_POLICY TECHNICAL_VERDICT HUMAN_OWNER_VERDICT').split()
    for r in data.get('risk', []):
        require(all(k in r for k in required_risk), 'missing risk fields')
        require(r.get('EXTERNAL_USE') == 'UNKNOWN', 'external use inference forbidden')
        require(r.get('HUMAN_OWNER_VERDICT') == 'PENDING', 'unsigned approval forbidden')
    require(len(data.get('risk', [])) == 17, '17 risk dispositions required')
    require(data.get('owner_approvals') == {'508':'PENDING','510':'PENDING'}, 'owner authorization absent')
    return errors


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--baseline'); ap.add_argument('--candidate-508'); ap.add_argument('--candidate-510')
    ap.add_argument('--matrix',action='store_true'); ap.add_argument('--output',type=Path)
    ap.add_argument('--verify-evidence',type=Path)
    ap.add_argument('--workspace',type=Path,default=REPO.parents[1])
    args=ap.parse_args()
    if args.verify_evidence:
        data=json.loads((args.verify_evidence/'evidence.json').read_text())
        errors=verify(data,args.verify_evidence)
        print(json.dumps({'mode':'INTEGRITY_ONLY_NO_EXECUTION','errors':errors,'status':'FAIL' if errors else 'PASS'},indent=2))
        return bool(errors)
    if not all((args.baseline,args.candidate_508,args.candidate_510,args.matrix,args.output)):
        ap.error('baseline, candidates, matrix and output are required')
    ex=Experiment(args.output)
    verifier_snapshot=ex.output/'verifier-source'
    verifier_snapshot.mkdir()
    (verifier_snapshot/'verify_f4_retirement.py').write_bytes(Path(__file__).read_bytes())
    (verifier_snapshot/'surface_manifest.json').write_bytes(MANIFEST.read_bytes())
    manifest=json.loads(MANIFEST.read_text())
    data=dict(schema=1,generated_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
              verifier_sha=git(ex,REPO,'rev-parse','HEAD'),verifier_script_sha256=digest(Path(__file__)),manifest=manifest,inputs={},trees={},packages=[],commands=ex.commands,risk=[],
              owner_approvals={'508':'PENDING','510':'PENDING'},optional_w04_supported=False)
    ids={'B0':('baseline',args.baseline),'H0':('historical-v001',None),
         'C508':('candidate-508',args.candidate_508),'C510':('candidate-510',args.candidate_510),
         'C508510':('integration-508-510',None)}
    try:
        for name,argv in [('compiler',['g++','--version']),('clang',['clang++','--version']),
                          ('xmake',['xmake','--version']),('kernel',['uname','-a']),('liburing',['pkg-config','--modversion','liburing']),
                          ('loader',['ldconfig','-p']),('ar',['ar','--version']),('nm',['nm','--version'])]:
            c,_,_=ex.run(argv); data[name]=c['id']
        for tid,(directory,requested) in ids.items():
            tree=args.workspace/'worktrees'/directory
            actual=git(ex,tree,'rev-parse','HEAD')
            if requested and requested!=actual:
                raise RuntimeError(f'input SHA mismatch {tid}: {requested} != {actual}')
            if git(ex,tree,'status','--porcelain=v1'):
                raise RuntimeError(f'dirty tree {tid}')
            data['inputs'][tid]=requested or actual
            data['trees'][tid]=dict(sha=actual,tree=git(ex,tree,'rev-parse','HEAD^{tree}'),path=str(tree),input_hashes=input_hashes(ex,tree))
            if tid != 'H0':
                data['trees'][tid]['base_diff']=git(ex,tree,'diff','--name-status',args.baseline,'HEAD')
                data['trees'][tid]['merge_base']=git(ex,tree,'merge-base',args.baseline,'HEAD')
        historical=args.workspace/'worktrees/historical-v001'
        data['historical']={}
        history_dir=ex.output/'historical'; history_dir.mkdir()
        for doc in ('README.md','docs/reference/api.md','xmake.lua','xmake/libraries.lua'):
            path=historical/doc
            if path.exists(): (history_dir/doc.replace('/','__')).write_bytes(path.read_bytes())
        for s in manifest['surfaces']:
            src=historical/'include'/s['header']
            row=dict(present=src.exists(),source_sha256=digest(src) if src.exists() else None)
            if src.exists():
                saved=history_dir/(s['id']+'.hpp'); saved.write_bytes(src.read_bytes())
                unit=history_dir/(s['id']+'.cpp'); unit.write_text(f'#include <{s["header"]}>\n')
                c,_,_=ex.run(['g++','-std=c++20',f'-I{historical}/include','-fsyntax-only',unit])
                row.update(standalone=c['id'],standalone_status=status(c),public_includes=re.findall(r'#include <(sluice/[^>]+)>',src.read_text()),types=s['types'],callables=s['callables'],description='See verbatim H0 API reference and README; no per-header uniform stability claim; H0 lacks package header installation rules')
                if s['parent']:
                    unit.write_text(f'#include <{s["parent"]}>\n#include <{s["header"]}>\n')
                    parent,_,_=ex.run(['g++','-std=c++20',f'-I{historical}/include','-fsyntax-only',unit])
                    row.update(parent_include=parent['id'],parent_status=status(parent))
            data['historical'][s['id']]=row
        for tid in ('B0','C508','C510','C508510'):
            tree=args.workspace/'worktrees'/ids[tid][0]
            for profile in ('no-liburing','real-liburing'):
                for mode in ('debug','release'):
                    data['packages'].append(package(ex,tree,tid,profile,mode,manifest))
        for p in data['packages']:
            if p['tree_id']=='B0': continue
            baseline=next(b for b in data['packages'] if b['tree_id']=='B0' and b['profile']==p['profile'] and b['mode']==p['mode'])
            deleted=['blocking_io_pool'] if p['tree_id']=='C508' else ['buffer','copy','copy_strategy','fault','file','io_context','observed','reader','wal','writer']
            if p['tree_id']=='C508510': deleted=['blocking_io_pool',*deleted]
            # Strong definitions from removed objects must disappear; inline template
            # instantiations and shared weak definitions are not callable ABI owners.
            cmd=ex.commands[baseline['archives']['core']['defined']]
            nm=(ex.output/cmd['stdout']).read_text()
            section=None; removed=set()
            for line in nm.splitlines():
                if line.endswith(':'): section=line[:-1]
                elif section and any(re.search(r'(^|[/.])'+re.escape(n)+r'\.cpp',section) for n in deleted):
                    if re.match(r'^[0-9a-fA-F]+ T ',line): removed.add(line.split(None,2)[2])
            all_defined='\n'.join((ex.output/ex.commands[p['archives'][lib]['defined']]['stdout']).read_text() for lib in ('core','async'))
            remain=sorted(removed & symbol_names(all_defined))
            p['retired_symbols']=dict(expected_absent=sorted(removed),remaining=remain,status='PASS' if removed and not remain else 'FAIL')
            p['negative_links']={}
            for s in manifest['surfaces']:
                if not p['surfaces'][s['id']]['removed'] or not s['link_negative']: continue
                obj=ex.output/'build'/f"B0/{p['profile']}/{p['mode']}"/(s['id']+'-api.o')
                target=ex.output/'build'/f"{p['tree_id']}/{p['profile']}/{p['mode']}"/(s['id']+'-negative')
                c,_,err=ex.run(['g++','-pthread',obj,f'-L{p["prefix"]}/lib','-lsluice_core','-o',target])
                p['negative_links'][s['id']]=dict(command=c['id'],status='PASS' if c['exit_code'] not in (0,None) and 'undefined reference' in err else 'FAIL')
        for s in manifest['surfaces']:
            old=[p for p in data['packages'] if p['tree_id']=='B0']
            candidates=[p for p in data['packages'] if p['tree_id'] in (f"C{s['group']}",'C508510')]
            refs=lambda ps,field: {f"{p['tree_id']}/{p['profile']}/{p['mode']}":p['surfaces'][s['id']].get(field) for p in ps}
            c, consumers, _=ex.run(['git','-C',args.workspace/'worktrees/baseline','grep','-n','-F',s['header'],'--','include','src','apps','tests'])
            risk=dict(SURFACE_ID=s['id'],HEADER=s['header'],V001_PUBLIC_FACT=data['historical'][s['id']],
                      CURRENT_INSTALLED_FACT=refs(old,'present'),CURRENT_IN_TREE_CONSUMER=dict(command=c['id'],matches=consumers),
                      EXTERNAL_USE='UNKNOWN',OLD_API_COMPILE={k:v.get('compile_status') for k,v in refs(old,'api').items()},
                      OLD_API_LINK={k:v.get('link_status') for k,v in refs(old,'api').items()},
                      OLD_BEHAVIOR_WITNESS={k:v.get('run_status') for k,v in refs(old,'api').items()},
                      CANDIDATE_HEADER_ABSENT={k:not v for k,v in refs(candidates,'present').items()},
                      CANDIDATE_API_COMPILE_REJECTED=refs(candidates,'negative_status'),
                      CANDIDATE_SYMBOL_ABSENT='HEADER_ONLY_NO_EXCLUSIVE_CALLABLE_ABI' if not s['link_negative'] else
                      {f"{p['tree_id']}/{p['profile']}/{p['mode']}":dict(symbol_audit=p['retired_symbols']['status'],link_negative=p['negative_links'][s['id']]['status']) for p in candidates},
                      RETAINED_CANONICAL_WORKLOAD={f"{p['tree_id']}/{p['profile']}/{p['mode']}":p['consumers'] for p in candidates},
                      MIGRATION_OR_NO_EQUIVALENT=s['migration'],
                      BREAKAGE_CONSEQUENCE='source include and named types/callables cease to compile; existing linked legacy consumers may fail to relink',
                      PROPOSED_POLICY='ACCEPT_BREAKAGE_NOW',TECHNICAL_VERDICT='PENDING_FULL_EVIDENCE_CHECK',HUMAN_OWNER_VERDICT='PENDING')
            data['risk'].append(risk)
    except Exception as e:
        data['blocked']=str(e)
        print(f'BLOCKED: {e}',file=sys.stderr)
    (ex.output/'evidence.json').write_text(json.dumps(data,indent=2)+'\n')
    errors=verify(data,ex.output)
    for risk in data['risk']:
        risk['TECHNICAL_VERDICT']='TECHNICALLY_SAFE_FOR_REQUIRED_V1_SCOPE' if not errors else 'BLOCK_AND_REVISE'
    (ex.output/'evidence.json').write_text(json.dumps(data,indent=2)+'\n')
    (ex.output/'REPORT.md').write_text('# F4 technical evidence\n\nStatus: '+('PASS' if not errors else 'FAIL')+'\n\n'+ '\n'.join(f"- {r['SURFACE_ID']} `{r['HEADER']}`: {r['TECHNICAL_VERDICT']}; external use UNKNOWN; human owner PENDING; migration: {r['MIGRATION_OR_NO_EQUIVALENT']}" for r in data['risk'])+'\n\nFull per-interface facts, commands, exit codes and raw log hashes: evidence.json. W-04 support: NO.\n')
    (ex.output/'verification.json').write_text(json.dumps({'errors':errors,'status':'FAIL' if errors else 'PASS'},indent=2)+'\n')
    (ex.output/'REPRODUCE.md').write_text('# F4 experiment\n\n'+shlex.join(sys.argv)+'\n\nIntegrity only: `python3 scripts/verify_f4_retirement.py --verify-evidence '+str(ex.output)+'`\n\n'+ '\n'.join(f"{c['id']}: cwd={c['cwd']} rc={c['exit_code']} `{shlex.join(c['argv'])}`" for c in ex.commands)+'\n')
    print(json.dumps({'output':str(ex.output),'packages':len(data['packages']),'errors':errors},indent=2))
    return bool(errors)


if __name__=='__main__':
    sys.exit(main())
