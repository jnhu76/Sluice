# aligne1-validate-native-4 — notes

INVALID attempt (retained immutable): harness bug 4 — perf -x, parse
assumed `<event>,<value>`; this perf (7.1.9) emits `<value>,<unit>,
<event>`, so all counters parsed as None (missing-value gate fired). 48
runs, 48 gate errors; bench runs themselves fine. Superseded by
aligne1-validate-native-6.