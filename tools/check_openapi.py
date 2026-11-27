#!/usr/bin/env python3
"""Checks that docs/openapi.yaml and the REST routes in src/app/api_bridge.cpp describe the same operations.

Compares the `add_route("METHOD", "/path"` pairs in the code with the `paths:` section of the YAML, both ways, and
prints what is missing on either side. Exit status 0 when they match, 1 otherwise. Standard library only (the YAML
is read with a small indentation-based scan of the `paths:` block, which is all this needs).

    python3 tools/check_openapi.py
"""
import re
import shutil
import subprocess
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


def strict_parse():
    """Parses the whole spec with a strict YAML parser when one is installed (PyYAML, else Ruby's Psych).
    Returns an error text, or None when it parses or no parser is available (nothing is installed for this)."""
    try:
        import yaml  # type: ignore
        try:
            yaml.safe_load(SPEC.read_text(encoding="utf-8"))
        except yaml.YAMLError as e:
            return str(e)
        return None
    except ImportError:
        pass
    ruby = shutil.which("ruby")
    if ruby:
        r = subprocess.run([ruby, "-ryaml", "-e", "YAML.safe_load(File.read(ARGV[0]))", str(SPEC)], capture_output=True, text=True)
        if r.returncode != 0:
            return (r.stderr or "").strip().splitlines()[0] if r.stderr.strip() else "ruby could not parse the file"
        return None
    print("note: no strict YAML parser (PyYAML or ruby) here; skipped the syntax check", file=sys.stderr)
    return None


def main():
    bad_yaml = strict_parse()
    if bad_yaml:
        print(f"openapi.yaml is not valid YAML: {bad_yaml}")
        return 1
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
