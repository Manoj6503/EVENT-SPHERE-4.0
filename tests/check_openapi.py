#!/usr/bin/env python3
"""Fails when docs/openapi.yaml and the routes in backend/src/main.cpp drift apart (every route documented, nothing invented).
Standard library only; if `openapi-spec-validator` and `pyyaml` are installed the spec is also validated against the OpenAPI 3 schema."""
import argparse, os, re, sys
ap = argparse.ArgumentParser(); ap.add_argument('--root', default='.'); A = ap.parse_args()
src = open(os.path.join(A.root, 'backend/src/main.cpp')).read()
spec = open(os.path.join(A.root, 'docs/openapi.yaml')).read()
norm = lambda p: re.sub(r'\([^()]*\)|\{[^}]*\}', '{}', p)
code = set()
for m in re.finditer(r'route\((true|false),\s*(?:R"\((.*?)\)"|"(.*?)")\s*,', src):
    code.add(('post' if m.group(1) == 'true' else 'get', norm(m.group(2) or m.group(3))))
for m in re.finditer(r'svr\.(Get|Post)\((?:R"\((.*?)\)"|"(.*?)")\s*,', src):
    code.add((m.group(1).lower(), norm(m.group(2) or m.group(3))))
doc, cur = set(), None
for line in spec.splitlines():
    m = re.match(r'^  (/\S+):\s*$', line)
    if m: cur = norm(m.group(1)); continue
    m = re.match(r'^    (get|post):\s*$', line)
    if m and cur: doc.add((m.group(1), cur))
bad = False
for x in sorted(code - doc): print('route missing from openapi.yaml:', *x); bad = True
for x in sorted(doc - code): print('openapi.yaml documents a route that does not exist:', *x); bad = True
print('%d routes in code, %d documented' % (len(code), len(doc)))
try:
    import yaml
    d = yaml.safe_load(spec)
    try:
        from openapi_spec_validator import validate_spec; validate_spec(d); print('OpenAPI 3 schema: valid')
    except ImportError: print('(openapi-spec-validator not installed: structural check skipped)')
except ImportError: print('(pyyaml not installed: schema check skipped)')
except Exception as e: print('OpenAPI schema error:', e); bad = True
sys.exit(1 if bad else 0)
