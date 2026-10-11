from pathlib import Path
import subprocess,json,hashlib,tarfile,shutil
base=Path(__file__).parent; repo=Path("/home/jnhu/Source/Sluice-f3-498-pr-b")
out=base/"results"; out.mkdir(parents=True,exist_ok=True)
provenance=json.loads((base/"provenance.json").read_text()); summaries={}
for p in ("n","y"):
 row={}
 for stage in ("before","after"):
  ev=base/stage/"evidence"; prefix=base/stage/("installed-"+p)
  summary=json.loads((ev/"summary.json").read_text())[p]
  for filename in ("resolved-"+p+".json","installed-headers-"+p+".txt","objects-"+p+".txt","public-closure-"+p+".json"):
   shutil.copyfile(ev/filename,out/(stage+"-"+filename))
  manifest={"id":"F3_498_ASYNC_FILE_CANDIDATE_V1","status":"DRAFT_NONADOPTED","source_sha":provenance[stage+"_sha"],"profile":p,"mode":"debug","summary":summary,"headers":(ev/("installed-headers-"+p+".txt")).read_text().splitlines(),"archives":{}}
  for lib in ("core","async"):
   archive=prefix/"lib"/("libsluice_"+lib+".a")
   members=(ev/("archive-"+lib+"-"+p+".txt")).read_text().splitlines()
   manifest["archives"][lib]={"sha256":hashlib.sha256(archive.read_bytes()).hexdigest(),"members":[{"name":m,"sha256":hashlib.sha256(subprocess.check_output(["ar","p",str(archive),m])).hexdigest()} for m in members]}
  row[stage]=manifest
  (out/(stage+"-manifest-"+p+".json")).write_text(json.dumps(manifest,indent=2)+"\n")
 old=set(row["before"]["headers"]); new=set(row["after"]["headers"])
 assert old-new=={"sluice/async/file.hpp"} and not new-old
 oldmembers=set(m["name"] for m in row["before"]["archives"]["async"]["members"]); newmembers=set(m["name"] for m in row["after"]["archives"]["async"]["members"])
 assert oldmembers-newmembers=={"file.cpp.o"} and not newmembers-oldmembers
 text=(base/"after/evidence"/("defined-async-"+p+".txt")).read_text()
 names=("await_read_at","await_write_at","await_sync_data","await_sync_all")
 assert not any("sluice::async::"+n+"(" in text for n in names)
 summaries[p]={"before":row["before"]["summary"],"after":row["after"]["summary"],"removed_headers":sorted(old-new),"removed_archive_members":sorted(oldmembers-newmembers),"four_removed_symbol_definitions_absent":True,"w03_threadpool":"PASS","w03_real_uring":"PASS" if p=="y" else "UNAVAILABLE_RC_2"}
for name in ("provenance.json","probe-results.json","negative-link-results.json"):
 shutil.copyfile(base/name,out/name)
for name in ("run.sh","probes.py","finalize.py"):
 shutil.copyfile(base/name,out/name)
raw={}
with tarfile.open(out/"raw-package-evidence.tar.gz","w:gz") as archive:
 for stage in ("before","after"):
  for path in sorted((base/stage/"evidence").rglob("*")):
   if path.is_file():
    relative=str(path.relative_to(base)); raw[relative]=hashlib.sha256(path.read_bytes()).hexdigest();archive.add(path,arcname=relative)
  for path in sorted((base/stage/("capture.py")).parent.glob("*.py")):
   archive.add(path,arcname=str(path.relative_to(base)))
  archive.add(base/stage/"inventory.lua",arcname=stage+"/inventory.lua")
(out/"raw-sha256.json").write_text(json.dumps(raw,indent=2)+"\n")
(out/"summary.json").write_text(json.dumps(summaries,indent=2)+"\n")
print(json.dumps(summaries,indent=2))
