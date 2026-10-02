#!/usr/bin/env python3
"""Check the C port (src/mesh.c) against sagemcom5598.py on the same
meshdevices reply(s): builds test/host_mesh.c with the host compiler, runs
both, and compares extenders, devices and topology (with nicknames).

usage: compare_mesh.py meshdevices.json [more.json ...] [--nicknames FILE]
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(REPO))
from sagemcom5598 import Sagemcom5598, add_nicknames, load_nicknames  # noqa: E402

CJSON = HERE.parent / "managed_components" / "espressif__cjson" / "cJSON"
BINARY = HERE / "host_mesh"


def build() -> None:
    if not (CJSON / "cJSON.c").is_file():
        raise SystemExit(f"{CJSON} missing: run `pio run` once so the component is downloaded")
    subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g", "-fsanitize=address,undefined",
         f"-I{HERE.parent / 'src'}", f"-I{CJSON}",
         str(HERE / "host_mesh.c"), str(HERE.parent / "src" / "mesh.c"), str(CJSON / "cJSON.c"),
         "-o", str(BINARY)],
        check=True,
    )


def python_views(reply, nicknames) -> dict:
    client = Sagemcom5598()
    client._get_json = lambda path, params=None: json.loads(json.dumps(reply))  # fresh copy per call
    out = {
        "extenders": client.connected_extenders(),
        "devices": client.connected_devices(),
        "topology": client.topology(),
    }
    return json.loads(json.dumps(add_nicknames(out, nicknames)))


def diff(a, b, path="$"):
    if type(a) is not type(b) and not (isinstance(a, (int, float)) and isinstance(b, (int, float))):
        yield f"{path}: python {a!r} != c {b!r}"
    elif isinstance(a, dict):
        if list(a) != list(b):
            yield f"{path}: keys python {list(a)} != c {list(b)}"
        for k in a.keys() & b.keys():
            yield from diff(a[k], b[k], f"{path}.{k}")
    elif isinstance(a, list):
        if len(a) != len(b):
            yield f"{path}: length python {len(a)} != c {len(b)}"
        for i, (x, y) in enumerate(zip(a, b)):
            yield from diff(x, y, f"{path}[{i}]")
    elif a != b:
        yield f"{path}: python {a!r} != c {b!r}"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("replies", nargs="+", type=Path)
    parser.add_argument("--nicknames", type=Path)
    args = parser.parse_args()
    build()
    nicknames = load_nicknames(args.nicknames) if args.nicknames else {}
    failed = False
    for reply_path in args.replies:
        cmd = [str(BINARY), str(reply_path)] + ([str(args.nicknames)] if args.nicknames else [])
        c_out = json.loads(subprocess.run(cmd, check=True, capture_output=True, text=True).stdout)
        py_out = python_views(json.loads(reply_path.read_text()), nicknames)
        problems = list(diff(py_out, c_out))
        counts = f"{len(py_out['extenders'])} extenders, {len(py_out['devices'])} devices"
        print(f"{reply_path.name}: {'OK' if not problems else 'MISMATCH'} ({counts})")
        for p in problems[:20]:
            print("   ", p)
        failed |= bool(problems)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
