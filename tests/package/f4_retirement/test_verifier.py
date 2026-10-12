import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[3]
spec=importlib.util.spec_from_file_location('f4',ROOT/'scripts/verify_f4_retirement.py')
f4=importlib.util.module_from_spec(spec)
spec.loader.exec_module(f4)


class IntegrityTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory()
        self.root=Path(self.tmp.name)
        manifest=json.loads(f4.MANIFEST.read_text())
        sha='a'*40
        commands=[]
        def cmd(rc=0, out='', err='', argv=None):
            n=len(commands)
            paths=[]
            for stream,text in [('stdout',out),('stderr',err)]:
                p=self.root/f'{n}.{stream}';p.write_text(text);paths.append(p)
            commands.append(dict(id=n,argv=argv or ['fixture'],exit_code=rc,
                stdout=paths[0].name,stderr=paths[1].name,stdout_sha256=f4.digest(paths[0]),
                stderr_sha256=f4.digest(paths[1]),environment={}))
            return n
        positive=cmd()
        negative=cmd(1,err='undefined reference')
        unavailable=cmd(2)
        probe=dict(compile=positive,link=positive,run=positive,compile_status='PASS',link_status='PASS',run_status='PASS')
        trees={tid:dict(sha=sha,tree=sha,input_hashes={},path='/fixture/'+tid) for tid in ('B0','H0','C508','C510','C508510')}
        self.data=dict(schema=1,manifest=manifest,commands=commands,trees=trees,inputs={tid:sha for tid in trees},packages=[],risk=[dict(EXTERNAL_USE='UNKNOWN',HUMAN_OWNER_VERDICT='PENDING') for _ in range(17)],owner_approvals={'508':'PENDING','510':'PENDING'})
        for tid in ('B0','C508','C510','C508510'):
            for profile in ('no-liburing','real-liburing'):
                for mode in ('debug','release'):
                    counts=manifest['expected'][tid];archives={}
                    for lib,count in zip(('core','async'),counts):
                        members=[f'{i}.o' for i in range(count)]
                        member=cmd(out='\n'.join(members)+'\n')
                        nm=cmd(out='00000000 T retained()\n')
                        archives[lib]=dict(members=members,count=count,member_command=member,defined=nm,sha256='x')
                    surfaces={}
                    for s in manifest['surfaces']:
                        removed=tid=='C508510' or tid==f'C{s["group"]}'
                        surfaces[s['id']]=dict(removed=removed,present=not removed,include=negative if removed else positive,negative_status='PASS',standalone='PASS',parent_status='PASS',api=probe.copy())
                    consumer=dict(compile_link=positive,compile_link_status='PASS',runtime=dict(command=positive,status='PASS'))
                    external=dict(compile_link=positive,compile_link_status='PASS',threadpool=dict(command=positive,status='PASS'),uring=dict(command=unavailable,status='UNAVAILABLE'),no_scheduler_fiber='PASS')
                    self.data['packages'].append(dict(tree_id=tid,profile=profile,mode=mode,prefix=str(self.root/'missing'),source_sha=sha,input_hashes={},counts=counts.copy(),headers=[str(i) for i in range(counts[2])],archives=archives,surfaces=surfaces,retained=probe.copy(),consumers={'w01_direct':consumer.copy(),'w02_pipeline':consumer.copy(),'w03_external_loop':external},retired_symbols=dict(status='PASS',expected_absent=['retired()'],remaining=[]),negative_links={'F4-01':dict(command=negative,status='PASS')}))
        self.assertEqual(f4.verify(self.data,self.root),[])

    def tearDown(self): self.tmp.cleanup()

    def rejects(self, fragment):
        errors=f4.verify(self.data,self.root)
        self.assertTrue(any(fragment in e for e in errors),errors)

    def test_missing_seventeenth_surface(self):
        del self.data['packages'][0]['surfaces']['F4-17'];self.rejects('missing surface')

    def test_negative_unexpected_success(self):
        p=self.data['packages'][4];p['surfaces']['F4-01']['include']=0;self.rejects('negative include')

    def test_source_include_leak(self):
        self.data['commands'][0]['argv']=['g++','-I/fixture/worktrees/baseline/include'];self.rejects('source include leakage')

    def test_archive_count_lie(self):
        self.data['packages'][0]['archives']['core']['members'].pop();self.rejects('archive/header counts')

    def test_retired_symbol_retained(self):
        p=self.data['packages'][4];cid=p['archives']['core']['defined'];c=self.data['commands'][cid]
        log=self.root/c['stdout'];log.write_text('00000000 T retired()\n');c['stdout_sha256']=f4.digest(log);self.rejects('retired callable symbols')

    def test_uring_unavailable_labeled_pass(self):
        self.data['packages'][0]['consumers']['w03_external_loop']['uring']['status']='PASS';self.rejects('uring unavailable mislabeled')

    def test_log_tampering(self):
        (self.root/self.data['commands'][0]['stdout']).write_text('tampered');self.rejects('log hash')

    def test_input_head_mismatch(self):
        self.data['inputs']['C508']='b'*40;self.rejects('input/measurement mismatch')

    def test_unsigned_risk_approved(self):
        self.data['risk'][0]['HUMAN_OWNER_VERDICT']='APPROVED';self.rejects('unsigned approval')


if __name__=='__main__': unittest.main()
