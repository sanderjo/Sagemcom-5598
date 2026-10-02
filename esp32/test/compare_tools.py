#!/usr/bin/env python3
"""Check the C tool layer (src/tools.c with mesh.c, pyjson.c) against the
Python MCP server: both run every tool on the same saved router replies
(see capture_fixtures.py), the Python one in-process through a fake router.
Results must be equal field by field, in the same key order; a tool that
fails must fail on both sides.

usage: .venv/bin/python esp32/test/compare_tools.py FIXTURE_DIR [--nicknames FILE]
"""
import argparse
import asyncio
import contextlib
import json
import logging
import subprocess
import sys
from pathlib import Path
from urllib.parse import urlencode

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(REPO))
sys.path.insert(0, str(HERE))

from mcp import Client  # noqa: E402

import sagemcom5598  # noqa: E402
import sagemcom5598_mcp  # noqa: E402
from compare_mesh import CJSON, diff  # noqa: E402

BINARY = HERE / "host_tools"
CALLS = [("router_overview", {}), ("network_topology", {}), ("list_devices", {}),
         ("list_devices", {"include_inactive": True}), ("list_extenders", {}), ("wan_details", {}),
         ("ethernet_ports", {}), ("wifi_details", {}), ("firewall_details", {}), ("dhcp_details", {})]


def build() -> None:
    src = HERE.parent / "src"
    subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g", "-fsanitize=address,undefined",
         f"-I{src}", f"-I{CJSON}", str(HERE / "host_tools.c"), str(src / "tools.c"), str(src / "mesh.c"),
         str(src / "pyjson.c"), str(CJSON / "cJSON.c"), "-lm", "-o", str(BINARY)],
        check=True,
    )


def fixture_file(fixtures: Path, path: str) -> Path:
    return fixtures / (path.strip("/").replace("/", "_").replace("?", "_").replace("=", "-") + ".json")


async def python_results(fixtures: Path, nicknames: Path | None) -> list:
    @contextlib.contextmanager
    def fake_router():
        client = sagemcom5598.Sagemcom5598()

        def get_json(path, params=None):
            if params:
                path += "?" + urlencode(params)
            return json.loads(fixture_file(fixtures, path).read_text())
        client._get_json = get_json
        yield client

    sagemcom5598_mcp._router = fake_router
    if nicknames:
        sagemcom5598_mcp.load_nicknames = lambda: sagemcom5598.load_nicknames(nicknames)
    else:
        sagemcom5598_mcp.load_nicknames = lambda: {}
    results = []
    async with Client(sagemcom5598_mcp.mcp) as client:
        for name, args in CALLS:
            r = await client.call_tool(name, args)
            results.append({"__error__": r.content[0].text} if r.is_error else json.loads(r.content[0].text))
    return results


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("fixtures", type=Path)
    parser.add_argument("--nicknames", type=Path)
    args = parser.parse_args()
    logging.disable(logging.CRITICAL)  # the Python server logs a traceback per failing tool
    build()
    py = asyncio.run(python_results(args.fixtures, args.nicknames))
    failed = False
    for (name, call_args), p in zip(CALLS, py):
        cmd = [str(BINARY), str(args.fixtures), name, json.dumps(call_args)]
        if args.nicknames:
            cmd.append(str(args.nicknames))
        c = json.loads(subprocess.run(cmd, check=True, capture_output=True, text=True).stdout)
        label = f"{name}({', '.join(f'{k}={v}' for k, v in call_args.items())})"
        if "__error__" in p or "__error__" in c:
            ok = "__error__" in p and "__error__" in c
            print(f"{label}: {'both fail' if ok else 'MISMATCH'}  python={p.get('__error__')!r} c={c.get('__error__')!r}")
        else:
            problems = list(diff(p, c))
            ok = not problems
            print(f"{label}: {'OK' if ok else 'MISMATCH'}")
            for line in problems[:15]:
                print("   ", line)
        failed |= not ok
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
