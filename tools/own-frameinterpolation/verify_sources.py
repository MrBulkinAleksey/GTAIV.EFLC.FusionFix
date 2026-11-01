#!/usr/bin/env python3
"""Check the pinned subset without downloading or executing upstream build tools."""
import hashlib,json,pathlib,sys,urllib.request
ROOT=pathlib.Path(__file__).resolve().parents[2]/'external/fidelityfx-src'
def main():
 m=json.loads((ROOT/'provenance.json').read_text());bad=[]
 for name,expected in m['vendored_sha256'].items():
  if hashlib.sha256((ROOT/name).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=expected:bad.append(name)
 if '--upstream' in sys.argv:
  for name,expected in m['files'].items():
   data=urllib.request.urlopen(f"{m['repository'].replace('github.com','raw.githubusercontent.com')}/{m['commit']}/{m['upstream_prefix']}{name}").read()
   if hashlib.sha256(data).hexdigest()!=expected:bad.append('upstream:'+name)
 if bad:raise SystemExit('Source hash mismatch: '+', '.join(bad))
 print(f"Verified {len(m['vendored_sha256'])} vendored files; AMD {m['tag']} {m['commit']}")
if __name__=='__main__':main()
