import copy
import importlib.util
import json
from pathlib import Path
import sys

root=Path(sys.argv[1]).resolve()
repo=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(repo/'scripts'))
import verify_f4_postmerge as verifier
base=json.loads((root/'evidence-sealed.json').read_text())
assert verifier.verify(base,root)==[]
cases={
 'missing consumers':lambda d:d['packages'][0].update(consumers={}),
 'execution SHA':lambda d:d.update(master_execution_sha='0'*40),
 'tree SHA':lambda d:d.update(master_tree_sha='0'*40),
 'macro view':lambda d:d['packages'][0].update(macro_view=-1),
 'missing trace':lambda d:d['packages'][2].update(progress_traces={}),
 'object hash':lambda d:next(iter(d['packages'][0]['negative_links'].values())).update(object_sha256='0'*64),
 'same-object substitution':lambda d:d['commands'][next(iter(d['packages'][0]['negative_links'].values()))['candidate_link']]['argv'].__setitem__(2,'/wrong.o'),
 'archive count':lambda d:d['packages'][0]['archives']['core'].update(count=3),
 'archive hash':lambda d:d['packages'][0]['archives']['core'].update(sha256='0'*64),
 'duplicate removed sources':lambda d:d.update(removed_sources=[dict(path='src/wrong.cpp',present=False)]*11),
 'missing manifest':lambda d:d['manifest'].update(surfaces=[]),
 'source substitution':lambda d:next(iter(d['sealed_inputs'].values())).update(sha256='0'*64),
 'include leak':lambda d:d['commands'][d['packages'][0]['consumers']['w01_direct']['compile_link']]['argv'].insert(2,'-I/home/jnhu/Source/Sluice/include'),
 'environment leak':lambda d:d['commands'][d['packages'][0]['consumers']['w01_direct']['compile_link']]['environment'].update(CPATH='/old/include'),
 'raw hash':lambda d:d['commands'][0].update(stdout_sha256='0'*64),
}
results=[]
for name,mutate in cases.items():
 d=copy.deepcopy(base);mutate(d)
 try:errors=verifier.verify(d,root)
 except (KeyError,IndexError,FileNotFoundError):errors=['malformed input rejected']
 assert errors,name+' falsely accepted'
 results.append(dict(test=name,result='REJECTED',diagnostics=errors))
print(json.dumps({'controls':len(results),'status':'PASS','results':results},indent=2))
