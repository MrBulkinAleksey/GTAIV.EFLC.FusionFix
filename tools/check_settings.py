#!/usr/bin/env python3
"""Cross-checks the settings the code reads against settings/GTAIV.EFLC.FusionFix.json.

SettingsReader returns the code's default for a key the description lacks, so a key read in code with no JSON entry
is silently fixed at that default (no cfg, no menu). This finds those and the other quiet mismatches:
  ERROR  key read in code, not in the JSON
  ERROR  JSON default outside its min/max or not one of its choices
  ERROR  @label/@description with no string in the English translation
  WARN   default in code differs from the JSON default (the JSON one is used; the code one only reads like it)
  WARN   ReadInteger of a float setting (truncated)
  WARN   string missing in another translation
  INFO   JSON key no code reads (dead or read in a way this script misses)
Exit code 1 on any ERROR. Usage: tools/check_settings.py [--info]
"""
import json, re, sys, pathlib

root = pathlib.Path(__file__).resolve().parent.parent
desc = json.loads((root / 'settings/GTAIV.EFLC.FusionFix.json').read_text(encoding='utf-8'))
settings = {s['id']: s for s in desc['settings']}
langs = {p.name.split('.')[-2]: json.loads(p.read_text(encoding='utf-8'))
         for p in (root / 'settings').glob('GTAIV.EFLC.FusionFix.*.json')}

num = r'(-?[0-9.]+f?|true|false|[A-Za-z_:]+)'
lit = r'"([A-Za-z0-9_]+)"\s*,\s*"([A-Za-z0-9_]+)"'
patterns = [
    (re.compile(r'Read(Integer|Float|String)\(\s*' + lit + r'\s*,\s*' + num), 'read'),
    (re.compile(r'Ini(Float|Int|Flag)\(\s*' + lit + r'\s*,\s*&[\w:]+\s*,\s*' + num), 'table'),
]
# gpugrass reads colours through a lambda with section PROCEDURAL
colour = re.compile(r'colour\(\s*"([A-Za-z0-9_]+)"')
idLiteral = re.compile(r'"([A-Z][A-Za-z0-9_]*\.[A-Za-z][A-Za-z0-9_]*)"')

reads = {}      # id -> list of (file:line, kind, code default)
mentioned = set()
for path in sorted((root / 'source').rglob('*')):
    if path.suffix not in ('.ixx', '.cpp', '.hpp', '.h', '.hxx', '.inl'):
        continue
    text = path.read_text(encoding='utf-8', errors='replace')
    rel = path.relative_to(root)
    lineOf = lambda pos: text.count('\n', 0, pos) + 1
    for rx, _ in patterns:
        for m in rx.finditer(text):
            kind, sec, key, default = m.groups()
            # upstream's temporal.ixx reads [TEMPORAL]; temporallayer.ixx sets those from our [ANTIALIASING]
            if path.name == 'temporal.ixx' and sec == 'TEMPORAL':
                continue
            reads.setdefault(f'{sec}.{key}', []).append((f'{rel}:{lineOf(m.start())}', kind, default))
    if path.name == 'gpugrass.ixx':
        for m in colour.finditer(text):
            reads.setdefault(f'PROCEDURAL.{m.group(1)}', []).append((f'{rel}:{lineOf(m.start())}', 'String', None))
    mentioned.update(idLiteral.findall(text))
    # Keys also turn up as bare "Key" next to a section elsewhere (menus, debug server): count those as mentioned
    mentioned.update('?' + k for k in re.findall(r'"([A-Za-z][A-Za-z0-9_]+)"', text))

errors = warns = 0
def report(level, msg):
    global errors, warns
    errors += level == 'ERROR'
    warns += level == 'WARN'
    if level != 'INFO' or '--info' in sys.argv:
        print(f'{level:5} {msg}')

def asNumber(v):
    if v is None:
        return None
    v = v.rstrip('f')
    if v in ('true', 'false'):
        return float(v == 'true')
    try:
        return float(v)
    except ValueError:
        return None   # a named constant

for id, uses in sorted(reads.items()):
    s = settings.get(id)
    where = uses[0][0]
    if not s:
        report('ERROR', f'{id}: read at {where}, not in the JSON (always the code default)')
        continue
    for loc, kind, default in uses:
        jd = s.get('default')
        cd = asNumber(default)
        if cd is not None and isinstance(jd, (int, float)) and abs(cd - float(jd)) > 1e-6:
            report('WARN', f'{id}: code default {default} at {loc}, JSON default {jd}')
        if kind == 'Integer' and s.get('type') == 'float':
            report('WARN', f'{id}: ReadInteger at {loc} of a float setting')

for id, s in settings.items():
    d = s.get('default')
    if isinstance(d, (int, float)) and not isinstance(d, bool):
        if 'min' in s and d < s['min'] or 'max' in s and d > s['max']:
            report('ERROR', f'{id}: default {d} outside [{s.get("min")}, {s.get("max")}]')
    if s.get('type') == 'choice' and d not in [c['value'] for c in s.get('choices', [])]:
        report('ERROR', f'{id}: default {d} is not one of its choices')
    if id not in reads and id not in mentioned and '?' + id.split('.', 1)[1] not in mentioned:
        report('INFO', f'{id}: in the JSON, no code reads it')

def strings(node):
    if isinstance(node, dict):
        for v in node.values():
            yield from strings(v)
    elif isinstance(node, list):
        for v in node:
            yield from strings(v)
    elif isinstance(node, str) and node.startswith('@'):
        yield node[1:]

needed = set(strings(desc))
for name in sorted(needed):
    if name not in langs.get('en', {}):
        report('ERROR', f'@{name}: no English string')
for lang, table in sorted(langs.items()):
    if lang == 'en':
        continue
    missing = sorted(n for n in needed if n not in table)
    if missing:
        report('WARN', f'{lang}: {len(missing)} strings missing, e.g. {", ".join(missing[:5])}')

print(f'{len(reads)} keys read in code, {len(settings)} in the JSON: {errors} errors, {warns} warnings')
sys.exit(1 if errors else 0)
