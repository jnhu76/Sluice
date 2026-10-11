import re


def successful_creations(trace):
    syscall = re.compile(r"(?:\bclone3?\(|<\.\.\. clone3? resumed>)")
    result = re.compile(r"=\s+([1-9][0-9]*)\s*$")
    return sum(bool(syscall.search(line) and result.search(line))
               for line in trace.splitlines())
