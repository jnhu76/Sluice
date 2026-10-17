#!/usr/bin/env python3
import json, pathlib, subprocess, time

SUITES = ["uring_progress_race_test", "backend_conformance_e1_threadpool_test",
          "threadpool_external_loop_test"]
TREES = {"f2": "/home/jnhu/Source/Sluice-f2",
         "baseline": "/home/jnhu/Source/Sluice-f2-baseline"}
ROUNDS = 4
COPIES = 10
OUT = pathlib.Path("/home/jnhu/f2-execution/final-stress-20261013")

env = {
    "f2_head": subprocess.run(["git","-C",TREES["f2"],"rev-parse","HEAD"],
                              capture_output=True,text=True).stdout.strip(),
    "baseline_head": subprocess.run(["git","-C",TREES["baseline"],"rev-parse","HEAD"],
                                    capture_output=True,text=True).stdout.strip(),
    "rounds": ROUNDS, "copies_per_round": COPIES,
    "cpus": "0,1", "mode": "debug", "liburing": "y",
    "nproc": subprocess.run(["nproc"],capture_output=True,text=True).stdout.strip(),
    "uname": subprocess.run(["uname","-r"],capture_output=True,text=True).stdout.strip(),
}
(OUT/"environment.json").write_text(json.dumps(env,indent=2)+"\n")

results = {s: {t: {"runs":0,"failures":[],"passes":0} for t in TREES} for s in SUITES}
for suite in SUITES:
    for rnd in range(1, ROUNDS+1):
        order = ["f2","baseline"] if rnd % 2 == 1 else ["baseline","f2"]
        for tree in order:
            binary = TREES[tree]+"/build/linux/x86_64/debug/"+suite
            procs = []
            for i in range(COPIES):
                log = OUT/suite/f"r{rnd}-{tree}-c{i}.log"
                log.parent.mkdir(parents=True, exist_ok=True)
                f = open(log,"w")
                p = subprocess.Popen(["taskset","-c","0,1",binary],
                                     stdout=f, stderr=subprocess.STDOUT)
                procs.append((p,f,str(log)))
            started = time.time()
            for p,f,log in procs:
                code = p.wait(); f.close()
                results[suite][tree]["runs"] += 1
                if code == 0:
                    results[suite][tree]["passes"] += 1
                else:
                    results[suite][tree]["failures"].append(
                        {"round":rnd,"copy":log,"exit":code})
            print(f"{suite} r{rnd} {tree}: "
                  f"{time.time()-started:.1f}s done", flush=True)
(OUT/"results.json").write_text(json.dumps(
    {"environment":env,"results":results},indent=2)+"\n")
verdict = {"F2_FINAL_STRESS": "PASS" if not any(
    r["failures"] for s in results.values() for r in s.values()) else "FAIL"}
print(json.dumps(verdict), flush=True)
