#!/usr/bin/env python3
import argparse, hashlib, json, pathlib, re, shutil, subprocess, tarfile
ap=argparse.ArgumentParser()
ap.add_argument("--repo",required=True); ap.add_argument("--output-dir",required=True)
ap.add_argument("--before",required=True); ap.add_argument("--after",required=True)
a=ap.parse_args(); repo=pathlib.Path(a.repo).resolve(); base=pathlib.Path(a.output_dir).resolve(); assets=pathlib.Path(__file__).parent
if base.exists(): raise SystemExit("output directory must be fresh and absent")
base.mkdir(parents=True)
def git(*args): return subprocess.check_output(["git","-C",str(repo),*args])
before=git("rev-parse",a.before).decode().strip(); after=git("rev-parse",a.after).decode().strip()
with tarfile.open(assets/"raw-package-evidence.tar.gz") as ar:
 for stage in ("before","after"):
  root=base/stage; (root/"evidence").mkdir(parents=True)
  source=ar.extractfile(stage+"/capture.py").read().decode()
  source=re.sub(r"^base=.*$","base=pathlib.Path("+repr(str(root))+")",source,flags=re.M)
  (root/"capture.py").write_text(source)
  (root/"inventory.lua").write_bytes(ar.extractfile(stage+"/inventory.lua").read())
run=(assets/"run.sh").read_text().replace("/home/jnhu/f3-498-pr-b-prep",str(base)).replace("/home/jnhu/Source/Sluice-f3-498-pr-b",str(repo)).replace("ref=a9165f0a","ref="+before).replace("ref=94507a57c71704513497ecee3cc01d2bed3b1f24","ref="+after)
(base/"run.sh").write_text(run)
for name in ("probes.py","finalize.py"):
 text=(assets/name).read_text().replace("/home/jnhu/Source/Sluice-f3-498-pr-b",str(repo))
 if name=="finalize.py": text=text.replace("out=repo/\"docs/evidence/f3-498-async-file-candidate-v1\"","out=base/\"results\"")
 (base/name).write_text(text)
subprocess.run(["bash",str(base/"run.sh")],check=True)
subprocess.run(["python3",str(base/"probes.py")],check=True)
provenance={"before_sha":before,"after_sha":after,"capture_source":"fresh git archive exact before/after SHAs","equivalence":[]}
for stage,sha in (("before",before),("after",after)):
 for profile in ("n","y"):
  root=base/stage/"profiles"/profile
  tracked=git("ls-tree","-r","--name-only",sha,"src","include","xmake","xmake.lua").decode().splitlines()
  for path in tracked:
   assert (root/path).read_bytes()==git("show",sha+":"+path),path
  actual=sorted(str(p.relative_to(root)) for d in ("src","include","xmake") for p in (root/d).rglob("*") if p.is_file())+["xmake.lua"]
  assert set(actual)==set(tracked),(stage,profile,"unexpected source/build input")
  provenance["equivalence"].append({"stage":stage,"profile":profile,"sha":sha,"tracked_inputs":len(tracked),"all_tracked_production_and_build_inputs_byte_identical":True})
(base/"provenance.json").write_text(json.dumps(provenance,indent=2)+"\n")
header=git("show",before+":include/sluice/async/file.hpp").decode().replace("#pragma once\n","")
results={}; w03={}
for stage in ("before","after"):
 for p in ("n","y"):
  prefix=base/stage/("installed-"+p); clean=base/stage/("clean-consumers-"+p); ev=base/stage/"evidence"; row={}
  for name in ("await_read_at","await_write_at","await_sync_data","await_sync_all"):
   src=clean/(name+"-link.cpp"); src.write_text(header+"\nauto* volatile old_address = &sluice::async::"+name+";\nint main() { return old_address == nullptr; }\n")
   args=["g++","-std=c++20","-I"+str(prefix/"include")]+(["-DSLUICE_HAS_LIBURING"] if p=="y" else [])+[str(src),str(prefix/"lib/libsluice_async.a"),str(prefix/"lib/libsluice_core.a"),"-pthread"]+(["-luring"] if p=="y" else [])+["-o",str(clean/(name+"-link"))]
   res=subprocess.run(args,cwd=clean,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
   (ev/("negative-symbol-link-"+name+"-"+p+".log")).write_text(res.stdout)
   (ev/("negative-symbol-link-"+name+"-"+p+".cpp")).write_bytes(src.read_bytes())
   assert (res.returncode==0)==(stage=="before"),(stage,p,name,res.stdout)
   if stage=="after": assert "undefined reference" in res.stdout,res.stdout
   row[name]={"command":args,"returncode":res.returncode,"source_sha256":hashlib.sha256(src.read_bytes()).hexdigest(),"result":"PRESENT" if stage=="before" else "ABSENT"}
  results[stage+"-"+p]=row
  args=[str(clean/"w03_external_loop"),"uring"]
  res=subprocess.run(args,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
  (ev/("run-w03-uring-"+p+".log")).write_text(res.stdout)
  assert res.returncode==(0 if p=="y" else 2),(stage,p,res.stdout)
  w03[stage+"-"+p]={"command":args,"returncode":res.returncode,"result":"REAL_URING_PASS" if p=="y" else "EXPLICIT_UNAVAILABLE"}
(base/"negative-link-results.json").write_text(json.dumps(results,indent=2)+"\n")
(base/"w03-uring-results.json").write_text(json.dumps(w03,indent=2)+"\n")
subprocess.run(["python3",str(base/"finalize.py")],check=True)
shutil.copyfile(base/"w03-uring-results.json",base/"results/w03-uring-results.json")
shutil.copyfile(__file__,base/"results/fresh_reproduce.py")
print("FRESH_REPRODUCTION_PASS",base/"results")
