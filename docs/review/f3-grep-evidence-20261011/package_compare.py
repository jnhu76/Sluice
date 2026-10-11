import hashlib,json,pathlib,subprocess
repo=pathlib.Path("/home/jnhu/Source/Sluice-f3-grep-direct")
e=pathlib.Path("/home/jnhu/Source/f3-grep-evidence")
base="0d580498e8485d44166dac3e1148efb7adf52770"
head=subprocess.check_output(["git","rev-parse","HEAD"],cwd=repo,text=True).strip()
def run(args,cwd=repo):
    return subprocess.check_output(args,cwd=cwd,text=True)
sdk=["include","src","xmake.lua","xmake/helpers.lua","xmake/libraries.lua"]
delta=run(["git","diff","--name-only",base,head,"--",*sdk]).splitlines()
assert not delta,delta
source=e/"baseline-source"
(source/"apps/sluice-grep").mkdir(parents=True,exist_ok=True)
for file in ("main.cpp","grep_task.cpp","grep_task.hpp","cli_parse.cpp","cli_parse.hpp","matcher.cpp","matcher.hpp"):
    (source/"apps/sluice-grep"/file).write_text(run(["git","show",base+":apps/sluice-grep/"+file]))
def closure(root):
    found=set()
    for file in ("main.cpp","grep_task.cpp","cli_parse.cpp","matcher.cpp"):
        deps=run(["g++","-std=c++20","-MM","-I"+str(repo/"include"),"-I"+str(root/"apps/sluice-grep"),str(root/"apps/sluice-grep"/file)])
        for item in deps.replace("\\\n"," ").split():
            if str(repo/"include/") in item:
                found.add(item.split(str(repo)+"/",1)[1])
    return sorted(found)
before=closure(source)
after=closure(repo)
old=e/"baseline-artifacts"
new=repo/"build-baseline/linux/x86_64/debug"
def archive(directory,name):
    file=directory/name
    return {"sha256":hashlib.sha256(file.read_bytes()).hexdigest(),
            "objects":run(["ar","t",str(file)]).splitlines(),
            "defined":run(["nm","-g","--defined-only",str(file)]).splitlines()}
libraries={}
for name in ("libsluice_core.a","libsluice_async.a"):
    a,b=archive(old,name),archive(new,name)
    assert a==b,name
    libraries[name]={"identical":True,"sha256":a["sha256"],"object_count":len(a["objects"]),"defined_symbol_lines":len(a["defined"])}
def linkage(directory):
    return [line.strip() for line in run(["readelf","-d",str(directory/"sluice-grep")]).splitlines() if "NEEDED" in line]
assert linkage(old)==linkage(new)
prefix=e/"installed-n"
for target in ("sluice_core","sluice_async"):
    subprocess.run(["xmake","install","-o",str(prefix),target],cwd=repo,check=True,stdout=subprocess.DEVNULL)
headers={}
for file in sorted((prefix/"include").rglob("*.hpp")):
    rel=file.relative_to(prefix)
    assert file.read_bytes()==(repo/rel).read_bytes(),str(rel)
    headers[str(rel)]=hashlib.sha256(file.read_bytes()).hexdigest()
assert len(headers)==79,len(headers)
record={"base":base,"head":head,"scope":"Debug noliburing identical archive objects/symbols/bytes and linkage; SDK input identity for both profiles; installed header identity. Not a new full B-01 freeze.",
        "sdk_input_delta":delta,"project_include_closure_before":before,"project_include_closure_after":after,
        "libraries":libraries,"binary_needed_before":linkage(old),"binary_needed_after":linkage(new),
        "installed_headers":len(headers),"installed_header_digest":hashlib.sha256(json.dumps(headers,sort_keys=True).encode()).hexdigest()}
(e/"package-equivalence.json").write_text(json.dumps(record,indent=2)+"\n")
print("PACKAGE_EQUIVALENCE PASS", "include closure",len(before),len(after),"headers",len(headers))
