#!/usr/bin/env python3
"""Re-applies a branch's changes to source/postfx.ixx onto the split post fx modules (the cleanup branch).

For a branch made before the split:
    git worktree add -b <branch>-cleanup ../fusionfix-<branch>-cleanup cleanup
    git merge <branch>                          # conflicts in source/postfx.ixx
    git checkout --ours source/postfx.ixx
    git diff -U3 $(git merge-base master <branch>)...<branch> -- source/postfx.ixx > postfx.diff
    python3 tools/port_postfx_split.py postfx.diff
Each hunk's old text (context and removed lines) is looked up in every source/*.ixx, as it is and with the PostFX
class's four spaces of indent taken off (members moved to namespace scope). A hunk found in exactly one place is
applied there; the others are printed for doing by hand: ini reads become rows of the IniSetting tables in
postfxresources, code whose context moved goes into the module that holds it now. Then add the imports a module
needs for what it got, and check `git diff --stat` adds up to the branch's own postfx.ixx diff.
Only the files it changes are written, with their line endings kept.
"""
import re, sys, glob

def parse(path):
    hunks, cur = [], None
    for line in open(path, encoding='utf-8').read().split('\n'):
        if line.startswith('@@'):
            cur = {'header': line, 'old': [], 'new': []}
            hunks.append(cur)
        elif cur is not None and line[:1] in (' ', '-', '+') and not line.startswith(('---', '+++')):
            tag, text = line[0], line[1:]
            if tag in ' -':
                cur['old'].append(text)
            if tag in ' +':
                cur['new'].append(text)
    return hunks

def dedent(lines):
    return [l[4:] if l.startswith('    ') else l for l in lines]

def find_all(text, block):
    needle = '\n'.join(block) + '\n'
    out, i = [], text.find(needle)
    while i >= 0:
        out.append(i)
        i = text.find(needle, i + 1)
    return out

files = sorted(glob.glob('source/*.ixx'))
raw = {f: open(f, encoding='utf-8', newline='').read() for f in files}
newline = {f: '\r\n' if '\r\n' in raw[f] else '\n' for f in files}
texts = {f: raw[f].replace('\r\n', '\n') for f in files}
original = dict(texts)
manual = []
for h in parse(sys.argv[1]):
    if not any(l.strip() for l in h['old']):
        manual.append((h, 'no context'))
        continue
    places = []
    for variant in (lambda x: x, dedent):
        old, new = variant(h['old']), variant(h['new'])
        for f in files:
            for pos in find_all(texts[f], old):
                places.append((f, pos, old, new))
        if places:
            break
    if len(places) > 1 and all(l.startswith('import ') or not l.strip() for l in h['old']):
        places = [p for p in places if p[0].endswith('/postfx.ixx')]
    if len(places) != 1:
        manual.append((h, f'{len(places)} places: ' + ', '.join(sorted({p[0] for p in places}))))
        continue
    f, pos, old, new = places[0]
    t = texts[f]
    texts[f] = t[:pos] + '\n'.join(new) + '\n' + t[pos + len('\n'.join(old)) + 1:]
    print(f'applied {h["header"].split("@@")[1].strip()} -> {f}')
for f in files:
    if texts[f] != original[f]:
        open(f, 'w', encoding='utf-8', newline='').write(texts[f].replace('\n', newline[f]))
for h, why in manual:
    print(f'\nBY HAND {h["header"]} ({why})')
    for l in h['new']:
        print('   ' + l)
