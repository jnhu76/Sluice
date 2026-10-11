from pathlib import Path
import hashlib,json,subprocess,tarfile,shutil
repo=Path("/home/jnhu/Source/Sluice-f3-498-pr-b"); base=Path("/home/jnhu/f3-498-execution"); out=repo/"docs/evidence/f3-498-pr-b-final-matrix"; out.mkdir(exist_ok=True)
summary={}; raw={}
for profile in ("debug-n","debug-y","release-n","release-y","asanubsan-n","tsan-n"):
 initial=base/"pr-b-matrix"/profile; rerun=base/"pr-b-rerun-scope-smoke"/profile
 rows=dict(line.split("\t") for line in (initial/"results.tsv").read_text().splitlines()); changed=dict(line.split("\t") for line in (rerun/"results.tsv").read_text().splitlines())
 original=dict(rows); rows.update(changed)
 if profile=="tsan-n":
  assert rows["request_scope_test"]=="LINK_UNAVAILABLE_STRONG_ALLOCATION_OVERRIDES"
  assert sum(x=="PASS" for x in rows.values())==12
 else: assert all(x=="PASS" for x in rows.values()),(profile,rows)
 expected=52 if profile in ("debug-y","release-y") else 40 if profile in ("debug-n","release-n") else 13
 assert len(rows)==expected,(profile,len(rows))
 cache=(initial/"toolchain-cache.txt").read_text(); assert "/usr/lib64/ccache" not in cache
 compiler="/usr/bin/clang++" if profile.startswith(("tsan","asan")) else "/usr/bin/g++"
 assert compiler in cache
 assert "ccache = false" in (initial/"xmake.conf").read_text()
 summary[profile]={"initial_sha":(initial/"sha.txt").read_text().strip(),"rerun_sha":(rerun/"sha.txt").read_text().strip(),"initial_results":original,"rerun_results":changed,"effective_results":rows,"actual_direct_compiler_program":compiler,"xmake_ccache_enabled":False,"effective_pass_count":sum(x=="PASS" for x in rows.values()),"total_targets":len(rows)}
 (out/(profile+"-results.json")).write_text(json.dumps(summary[profile],indent=2)+"\n")
with tarfile.open(out/"raw-matrix-evidence.tar.gz","w:gz") as ar:
 for family in ("pr-b-matrix","pr-b-rerun-scope-smoke"):
  for path in sorted((base/family).rglob("*")):
   if path.is_file() and "install" not in path.parts and path.suffix in (".log",".txt",".tsv",".sh",".conf"):
    relative=str(path.relative_to(base)); raw[relative]=hashlib.sha256(path.read_bytes()).hexdigest();ar.add(path,arcname=relative)
 (out/"raw-sha256.json").write_text(json.dumps(raw,indent=2)+"\n")
(out/"summary.json").write_text(json.dumps(summary,indent=2)+"\n")
shutil.copyfile(__file__,out/"collect-pr-b.py")
print({p:(x["effective_pass_count"],x["total_targets"]) for p,x in summary.items()})
