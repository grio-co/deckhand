#!/usr/bin/env python3
"""Checks that docs/openapi.yaml and the REST routes in src/app/api_bridge.cpp describe the same operations.

Compares the `add_route("METHOD", "/path"` pairs in the code with the `paths:` section of the YAML, both ways, and
prints what is missing on either side. Exit status 0 when they match, 1 otherwise. Standard library only (the YAML
is read with a small indentation-based scan of the `paths:` block, which is all this needs).

    python3 tools/check_openapi.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CODE = ROOT / "src" / "app" / "api_bridge.cpp"
SPEC = ROOT / "docs" / "openapi.yaml"
METHODS = {"get", "put", "post", "delete", "patch", "head", "options"}


def routes_in_code():
    text = CODE.read_text(encoding="utf-8")
    return {(m.group(1).lower(), m.group(2)) for m in re.finditer(r'add_route\(\s*"([A-Z]+)"\s*,\s*"([^"]+)"', text)}


def operations_in_spec():
    ops = set()
    in_paths = False
    path = None
    for line in SPEC.read_text(encoding="utf-8").splitlines():
        if re.match(r"^paths:\s*$", line):
            in_paths = True
            continue
        if in_paths and re.match(r"^\S", line) and not line.startswith("#"):
            break  # next top-level key
        if not in_paths:
            continue
        m = re.match(r"^  (/\S*):\s*$", line)
        if m:
            path = m.group(1)
            continue
        m = re.match(r"^    ([a-z]+):\s*$", line)
        if m and path and m.group(1) in METHODS:
            ops.add((m.group(1), path))
    return ops


def main():
    code, spec = routes_in_code(), operations_in_spec()
    if not code:
        print(f"no add_route calls found in {CODE}", file=sys.stderr)
        return 1
    missing_in_spec = sorted(code - spec, key=lambda r: (r[1], r[0]))
    missing_in_code = sorted(spec - code, key=lambda r: (r[1], r[0]))
    for method, path in missing_in_spec:
        print(f"in the code but not in openapi.yaml: {method.upper()} {path}")
    for method, path in missing_in_code:
        print(f"in openapi.yaml but not in the code: {method.upper()} {path}")
    if missing_in_spec or missing_in_code:
        return 1
    print(f"openapi.yaml and api_bridge.cpp agree on {len(code)} operations")
    return 0


if __name__ == "__main__":
    sys.exit(main())
