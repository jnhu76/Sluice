#!/usr/bin/env python3
"""Check the dated Phase M terminal projection against its adopted sources.

--write-markdown renders all JSON facts; --github also checks live phase receipts.
No runtime/package matrix is run or implied.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
STEM = ROOT / 'docs/review/m-phase-final-adoption-20261011'
DECISIONS = {'D-H1', 'D-H2', 'D-H3', 'D-H4', 'D-H5', 'D3a', 'D3b',
             'D-12', 'D-13', 'D4', 'U-03/U-04', 'F-W2-1'}
FAMILIES = {'M-R', 'M-H', 'M-F', 'M-A1', 'M-A2', 'M-A3', 'M-A4'}


def git(*args):
    return subprocess.check_output(['git', *args], cwd=ROOT, text=True).strip()


def load(path):
    return json.loads((ROOT / path).read_text(encoding='utf-8'))


def cell(value):
    if isinstance(value, (list, dict)):
        value = json.dumps(value, ensure_ascii=False)
    return str(value).replace('|', '&#124;').replace('\n', '<br>')


def render(data):
    lines = ['# Phase M final adoption — 2026-10-11', '',
             'Machine-readable twin: [m-phase-final-adoption-20261011.json](m-phase-final-adoption-20261011.json).', '',
             'This is the current terminal projection. Prior final-closeout and wave-2 files remain dated historical snapshots. '
             'Merge adopts this record; actual closure and exact merge/head receipts are recorded on the final PR and #458. '
             'No Phase F retirement or v1 release readiness is implied.', '']

    def visit(value, name, depth):
        lines.extend(['#' * min(depth, 6) + ' ' + name, ''])
        if isinstance(value, dict):
            scalars = {k: v for k, v in value.items() if not isinstance(v, (list, dict))}
            if scalars:
                lines.extend(['| Field | Value |', '|---|---|'])
                lines.extend(f'| {cell(k)} | {cell(v)} |' for k, v in scalars.items())
                lines.append('')
            for k, v in value.items():
                if isinstance(v, (list, dict)):
                    visit(v, k, depth + 1)
        elif isinstance(value, list) and value and all(isinstance(v, dict) for v in value):
            keys = list(dict.fromkeys(k for v in value for k in v))
            lines.extend(['| ' + ' | '.join(keys) + ' |', '| ' + ' | '.join('---' for _ in keys) + ' |'])
            lines.extend('| ' + ' | '.join(cell(v.get(k, '')) for k in keys) + ' |' for v in value)
            lines.append('')
        elif isinstance(value, list):
            lines.extend('- ' + cell(v) for v in value)
            lines.append('')
        else:
            lines.extend([cell(value), ''])

    visit(data, 'Adoption record', 2)
    return '\n'.join(lines)


def verify(data, github):
    assert data['repository'] == 'jnhu76/Sluice'
    assert len(data['decisions']) == 12 and {d['id'] for d in data['decisions']} == DECISIONS
    assert len(data['families']) == 7 and {f['id'] for f in data['families']} == FAMILIES
    for d in data['decisions']:
        for key in ('WHAT', 'WHY', 'AUTHORITY', 'CONSUMER', 'CURRENT_TREE_STATUS', 'V1_TARGET_STATUS',
                    'PRESERVED_BEHAVIOR', 'FAMILY_OWNER', 'EXIT_TRIGGER', 'EVIDENCE', 'UNRESOLVED_RISK'):
            assert d[key], (d['id'], key)
    for f in data['families']:
        assert f['DECISION_ADOPTED'] == 'YES' and f['CONSUMER_DISPOSITION_COMPLETE'] == 'YES'
        assert f['AUTHORITY_CONFLICTS'] == 0 and f['FAMILY_OWNER'] and f['DOWNSTREAM_EXIT_GATE']
    authority = data['authority']
    assert authority['root_revision'] == 'v1-r4' and authority['root_adoption'] == 'VERIFIED'
    assert '| Revision | v1-r4 |' in (ROOT / 'docs/explicit-io-v1-final-decision.md').read_text()
    assert git('merge-base', '--is-ancestor', authority['root_merge_sha'], 'HEAD') == ''
    for key in ('protected_blobs', 'source_blobs'):
        for path, blob in data['validation'][key].items():
            assert git('hash-object', path) == blob, path
    census = load('docs/review/m-consumer-edges.json')
    expected = {(r['consumer_id'], r['path']): r for e in census['EDGES'] for r in e['consumers']}
    actual = {(r['id'], r['path']): r for r in data['consumer_closure']['rows']}
    assert len(actual) == len(data['consumer_closure']['rows']) == len(expected) == 216
    assert actual.keys() == expected.keys()
    for key, row in actual.items():
        assert row['owner'] and row['exit_condition'] and row['semantic_scope'] == expected[key]['semantic_role']
    assert sum(r['family'] == 'M-R' for r in actual.values()) == 75
    assert {r['id'] for r in data['consumer_closure']['external_surfaces']} == {
        'U-01', 'U-02', 'U-03', 'U-04', 'U-05', 'U-06', 'U-07', 'U-08', 'U-POOL'}
    baseline = data['package_provenance']['profiles'][0]['production_baseline']
    assert not git('diff', '--name-only', baseline, '--', 'src', 'include')
    initial = authority['initial_master_sha']
    assert not git('diff', '--name-only', initial, '--', 'src', 'include', 'xmake', 'xmake.lua')
    for p in data['package_provenance']['profiles']:
        m = load(p['manifest'])
        digest = hashlib.sha256((ROOT / p['archive']).read_bytes()).hexdigest()
        assert digest == p['archive_sha256'] == p['manifest_bound_sha256'] == m['ARCHIVE_BASELINE']['sha256']
        assert len(m['HEADERS']) == p['installed_headers'] == 79
    v = data['verdict']
    for key in ('F2_RETIREMENT_COMPLETE', 'F3_RETIREMENT_COMPLETE', 'F4_RETIREMENT_COMPLETE',
                'F5_PACKAGE_CONTRACTION_COMPLETE', 'D2_IMPLEMENTATION_VERIFIED', 'W04_OPTIONAL_SUPPORTED', 'V1_RELEASE_READY'):
        assert v[key] == 'NO', key
    assert data['consumer_closure']['unowned_consumer_edges'] == 0
    if github:
        def fetch(endpoint):
            return json.loads(subprocess.check_output(['gh', 'api', 'repos/jnhu76/Sluice/' + endpoint], text=True))
        for n, sha in ((488, authority['root_merge_sha']), (490, data['package_provenance']['merge_sha'])):
            pr = fetch(f'pulls/{n}')
            assert pr['merged'] and pr['merge_commit_sha'] == sha
        assert fetch('issues/489')['state'] == 'closed'
        assert fetch('issues/402')['state'] == 'open'
        for n in (474, 461, 462):
            assert fetch(f'issues/{n}')['state'] == 'open'
    print('PASS: 12 decisions; 7 families; 216 exact census rows; 75 M-R rows; scoped package hashes; historical/source blobs; no production/package mutation')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--write-markdown', action='store_true')
    parser.add_argument('--github', action='store_true')
    args = parser.parse_args()
    data = json.loads(STEM.with_suffix('.json').read_text(encoding='utf-8'))
    verify(data, args.github)
    rendered = render(data)
    if args.write_markdown:
        STEM.with_suffix('.md').write_text(rendered, encoding='utf-8')
    assert STEM.with_suffix('.md').read_text(encoding='utf-8') == rendered, 'Markdown/JSON facts differ'
    print('MARKDOWN_JSON_CONSISTENCY = PASS')


if __name__ == '__main__':
    main()
