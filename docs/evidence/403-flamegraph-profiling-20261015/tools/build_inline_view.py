#!/usr/bin/env python3
"""Builds a single-file HTML view that inlines the four key flame graphs.

Usage: build_inline_view.py <output.html>

The inlined graphs are static: FlameGraph's own JavaScript collides when several
SVGs share one document (duplicate ids, document-scoped <style>), so the script
and the JS-only elements are stripped and the stylesheet is scoped per graph.
Native <title> hover tooltips are kept; svg/ holds the fully interactive copies.
"""
import gzip
import re
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from report_attribution import classify, iter_stacks  # noqa: E402

TOOLS_DIR = Path(__file__).resolve().parent
EVIDENCE_DIR = TOOLS_DIR.parent
DATA = EVIDENCE_DIR / "data"
SVG = EVIDENCE_DIR / "svg"
COMPACT = EVIDENCE_DIR / "svg-compact"
FLAMEGRAPH_DIR = TOOLS_DIR / "flamegraph"

CASES = [
    ("w02-tp-4k-d16-w4", "W-02 ThreadPool", "4 KiB · depth=16 · workers=4",
     "20000 次 4 KiB 顺序读，depth=16 滑动窗口；ThreadPool 后端"),
    ("w02-uring-4k-d16", "W-02 io_uring", "4 KiB · depth=16",
     "同一 workload，io_uring 后端；读由内核 io-wq 工作线程执行"),
    ("w03-tp-4k-d16-w4", "W-03 外部循环", "ThreadPool · 4 KiB · depth=16",
     "应用自己的 epoll 循环驱动 ProgressSource，ThreadPool 后端"),
    ("w01-read-4k-seq", "W-01 直接路径", "read_at 4 KiB 顺序读",
     "不经过 RequestCore / 线程池的直接阻塞调用，作为参照"),
]

SYSCALLS = {
    "w02-tp-4k-d16-w4": "9.7",
    "w02-uring-4k-d16": "7.1",
    "w03-tp-4k-d16-w4": "11.0",
    "w01-read-4k-seq": "3.3",
}

BUCKET_LABEL = {
    "kernel": "内核（含 syscall 机制）",
    "kernel (io-wq worker)": "内核 io-wq 工作线程",
    "libc pthread": "libc pthread / futex 用户态",
    "libc mem*": "libc mem*",
    "libc wrapper self": "libc syscall 封装自身",
    "sluice": "Sluice 用户态",
    "c++ runtime": "C++ 运行时",
    "bench harness": "benchmark 校验 / 计时",
    "other": "其他",
    "unresolved": "未解析",
}


def render_compact(name: str, script: Path) -> str:
    """A narrower flame graph for the inline page, with the JS-only parts removed."""
    COMPACT.mkdir(exist_ok=True)
    out = COMPACT / f"{name}.svg"
    with tempfile.NamedTemporaryFile("w", suffix=".folded") as folded:
        with gzip.open(DATA / "folded" / f"{name}.folded.gz", "rt") as source:
            folded.write(source.read())
        folded.flush()
        with out.open("w") as sink:
            subprocess.run(
                ["perl", str(script), "--title", name, "--width", "1000", "--height", "16",
                 "--fontsize", "12", "--minwidth", "0.7", "--countname", "cycles",
                 "--colors", "hot", folded.name],
                stdout=sink, check=True)
    text = out.read_text()
    text = re.sub(r"<\?xml.*?\?>\n", "", text, flags=re.S)
    text = re.sub(r"<!DOCTYPE svg.*?>\n", "", text, flags=re.S)
    text = re.sub(r"<script.*?</script>", "", text, flags=re.S)
    text = text.replace(' onload="init(evt)"', "")
    text = re.sub(r'width="[\d.]+" height="[\d.]+" viewBox=', 'viewBox=', text)
    text = re.sub(r'<text\s+x="[\d.]+" y="[\d.]+" ></text>\n', "", text)
    for eid in ("details", "unzoom", "search", "ignorecase", "matched", "title"):
        text = re.sub(rf'<text id="{eid}"[^>]*>[^<]*</text>\n', "", text)
    return text


def scope(text: str, index: int) -> str:
    """Uniquifies ids and scopes the embedded stylesheet to this one graph."""
    bg, frames = f"ff-bg-{index}", f"ff-frames-{index}"
    text = text.replace('id="background"', f'id="{bg}"').replace("url(#background)", f"url(#{bg})")
    text = text.replace('id="frames"', f'id="{frames}"')
    text = re.sub(r"<style type=\"text/css\">.*?</style>",
                  f'<style type="text/css">'
                  f'.ff-{index} text {{ font-family:Verdana; font-size:11px; fill:rgb(0,0,0); }}'
                  f'.ff-{index} #{frames} > *:hover {{ stroke:black; stroke-width:0.5; cursor:pointer; }}'
                  f'</style>', text, flags=re.S)
    return text.replace('<svg version="1.1" ', f'<svg class="ff-{index}" version="1.1" ')


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: build_inline_view.py <output.html>")
    out_path = Path(sys.argv[1])
    script = FLAMEGRAPH_DIR / "flamegraph.pl"
    if not script.is_file():
        raise SystemExit(f"missing {script}; clone the FlameGraph repository there")
    tabs, panels = [], []
    for index, (name, short, params, note) in enumerate(CASES):
        total = 0
        buckets = Counter()
        for frames, count in iter_stacks(DATA / "folded" / f"{name}.folded.gz"):
            b, _ = classify(frames)
            total += count
            buckets[b] += count
        shares = [(BUCKET_LABEL.get(b, b), 100.0 * c / total) for b, c in buckets.most_common()]
        shares = [(label, pct) for label, pct in shares if pct >= 0.4][:6]
        active = index == 0
        tabs.append(f'<button class="nav-link{" active" if active else ""}" id="tab-{name}" '
                    f'role="tab" aria-controls="panel-{name}" '
                    f'aria-selected="{"true" if active else "false"}" type="button">{short}</button>')
        share_rows = "".join(
            f'<div class="share-row"><span class="share-label">{label}</span>'
            f'<span class="share-bar" aria-hidden="true"><span style="width:{pct:.1f}%"></span></span>'
            f'<span class="share-value tabular-nums">{pct:.1f}%</span></div>'
            for label, pct in shares)
        panels.append(f'''
<div id="panel-{name}" role="tabpanel" aria-labelledby="tab-{name}"{"" if active else " hidden"}>
  <p class="case-note"><strong>{params}</strong> — {note}</p>
  <div class="viz-row case-stats">
    <span><span class="stat-label">syscalls/op</span><span class="tabular-nums">{SYSCALLS.get(name, "—")}</span></span>
  </div>
  <div class="fg-wrap">{scope(render_compact(name, script), index)}</div>
  <div class="share-list">{share_rows}</div>
</div>''')
    html = f'''<div id="sluice-flamegraphs">
<style>
#sluice-flamegraphs {{ color: var(--foreground); }}
#sluice-flamegraphs .case-note {{ color: var(--muted-foreground); font-size: var(--font-size-base); margin: 0 0 8px; }}
#sluice-flamegraphs .case-note strong {{ color: var(--foreground); font-weight: 500; }}
#sluice-flamegraphs .case-stats {{ gap: 16px; margin: 0 0 10px; font-size: var(--font-size-base); }}
#sluice-flamegraphs .stat-label {{ color: var(--muted-foreground); margin-right: 5px; }}
#sluice-flamegraphs .fg-wrap {{ background: #eeeeb0; border-radius: 6px; overflow-x: auto; }}
#sluice-flamegraphs .fg-wrap svg {{ display: block; width: 100%; min-width: 1000px; height: auto; }}
#sluice-flamegraphs .share-list {{ display: grid; gap: 5px; margin-top: 12px; }}
#sluice-flamegraphs .share-row {{ display: grid; grid-template-columns: minmax(140px, 14rem) 1fr auto; align-items: center; gap: 10px; }}
#sluice-flamegraphs .share-label {{ color: var(--muted-foreground); font-size: var(--font-size-base); }}
#sluice-flamegraphs .share-bar {{ display: block; height: 6px; border-radius: 3px; background: color-mix(in srgb, var(--muted-foreground) 20%, transparent); }}
#sluice-flamegraphs .share-bar > span {{ display: block; height: 100%; border-radius: 3px; background: var(--viz-series-1); }}
#sluice-flamegraphs .share-value {{ font-size: var(--font-size-base); min-width: 3.5rem; text-align: end; }}
@media (max-width: 560px) {{
  #sluice-flamegraphs .share-row {{ grid-template-columns: 1fr auto; }}
  #sluice-flamegraphs .share-bar {{ display: none; }}
}}
</style>
<div class="nav nav-pills" role="tablist" aria-label="火焰图用例">
{''.join(tabs)}
</div>
{''.join(panels)}
</div>'''
    out_path.write_text(html)
    print(f"wrote {out_path} ({out_path.stat().st_size / 1024:.0f} KiB)")


if __name__ == "__main__":
    main()
