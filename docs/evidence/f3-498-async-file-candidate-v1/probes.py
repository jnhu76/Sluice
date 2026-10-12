import pathlib, subprocess, json, hashlib, shutil
base=pathlib.Path(__file__).parent
repo=pathlib.Path("/home/jnhu/Source/Sluice-f3-498-pr-b")
results={}
names=("await_read_at","await_write_at","await_sync_data","await_sync_all")
for stage in ("before","after"):
 for profile in ("n","y"):
  key=stage+"-"+profile; ev=base/stage/"evidence"; prefix=base/stage/("installed-"+profile); clean=base/stage/("clean-consumers-"+profile); clean.mkdir(exist_ok=True)
  env={"PATH":"/usr/bin:/bin", "HOME":"/home/jnhu", "LANG":"C.UTF-8"}
  flags=["g++","-std=c++20","-g","-I"+str(prefix/"include")]+(["-DSLUICE_HAS_LIBURING"] if profile=="y" else [])
  commands=[]; r={}
  def command(args,label,expect=0):
   p=subprocess.run(args,cwd=clean,env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
   (ev/(label+"-"+profile+".log")).write_text(p.stdout); commands.append({"argv":list(map(str,args)),"returncode":p.returncode})
   assert (p.returncode==0)==(expect==0),(label,p.stdout)
  for consumer,libs in (("w01_direct",["-lsluice_core"]),("w02_pipeline",["-lsluice_async","-lsluice_core","-pthread"]),("w03_external_loop",["-lsluice_async","-lsluice_core","-pthread"])):
   source=clean/(consumer+".cpp"); source.write_bytes((base/stage/"profiles"/profile/"tests/package/consumers"/(consumer+".cpp")).read_bytes()); binary=clean/consumer
   command(flags+[str(source),"-L"+str(prefix/"lib")]+libs+(["-luring"] if profile=="y" and consumer!="w01_direct" else [])+["-Wl,-Map="+str(ev/(consumer+"-"+profile+".map")),"-o",str(binary)],"link-"+consumer)
   command(["nm","-C",str(binary)],"symbols-"+consumer)
   command([str(binary)]+(["threadpool"] if consumer=="w03_external_loop" else []),"run-"+consumer)
   r[consumer]="PASS"
  hdr=clean/"old-header.cpp"; hdr.write_text("#include <sluice/async/file.hpp>\nint main() {}\n")
  command(flags+["-fsyntax-only",str(hdr)],"probe-removed-header",0 if stage=="before" else 1)
  r["header"]="PRESENT" if stage=="before" else "ABSENT"
  for name in names:
   src=clean/(name+".cpp")
   src.write_text(("#include <sluice/async/file.hpp>\n" if stage=="before" else "#include <sluice/async/application_runtime.hpp>\n#include <sluice/file_resource.hpp>\n")+"auto removed = &sluice::async::"+name+";\nint main() {}\n")
   command(flags+["-fsyntax-only",str(src)],"probe-"+name,0 if stage=="before" else 1)
   r[name]="PRESENT" if stage=="before" else "ABSENT"
  (ev/("consumer-commands-"+profile+".json")).write_text(json.dumps(commands,indent=2)+"\n")
  results[key]=r
(base/"probe-results.json").write_text(json.dumps(results,indent=2)+"\n")
print(json.dumps(results,indent=2))
