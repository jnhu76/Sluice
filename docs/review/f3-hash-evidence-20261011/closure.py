import json
from pathlib import Path
import subprocess
roots={'baseline':Path('/home/jnhu/Source/sluice-f3-hash-base-no-20261011'), 'candidate':Path('/home/jnhu/Source/sluice-f3-hash-49858941-debug-n')}
out={}
for label, root in roots.items():
    item={}
    for source in ('main.cpp','cli_parse.cpp','hash_task.cpp'):
        cmd=['g++','-std=c++20','-MM','-Iinclude','-Iapps/sluice-hash', 'apps/sluice-hash/'+source]
        r=subprocess.run(cmd,cwd=root,capture_output=True,text=True)
        assert r.returncode == 0,r.stderr
        headers=sorted(set(x for x in r.stdout.replace('\\\n',' ').split() if x.startswith('include/sluice/')))
        item[source]={'command':cmd,'exit':r.returncode,'project_headers':headers,'project_header_count':len(headers),'runtime_header_reachable':'include/sluice/async/application_runtime.hpp' in headers}
    commands=json.loads((root/'compile_commands.json').read_text())
    files=[(Path(c.get('directory',root))/c['file']).resolve() for c in commands]
    item['resolved_production_sources']=sorted(set(str(f.relative_to(root)) for f in files if str(f).startswith(str(root/'src')+'/')))
    out[label]=item
print(json.dumps(out,indent=2))
