#!/usr/bin/env python3
"""End-to-end test of the ESP32 MCP server with the official MCP SDK client
(mode "auto": probes server/discover, falls back to the initialize handshake).

Compares tools/list with the Python server (sagemcom5598_mcp.py), then calls
each tool on the board and, right after, the same tool on the Python server
against the live router. Values like signal strength move between calls, so
results are compared on structure and identity: keys, MACs, hostnames,
nicknames. Run with the repo's .venv (needs the `mcp` package).

usage: .venv/bin/python esp32/test/e2e_mcp.py [--url http://sagemcom-mcp.local/mcp]
"""
import argparse
import asyncio
import configparser
import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))

from mcp import Client  # noqa: E402
from mcp.client.streamable_http import streamable_http_client  # noqa: E402
from mcp.shared._httpx_utils import create_mcp_http_client  # noqa: E402
from mcp.shared.exceptions import MCPError  # noqa: E402

import sagemcom5598_mcp  # noqa: E402

CALLS = [("router_overview", {}), ("network_topology", {}), ("list_extenders", {}), ("list_devices", {}),
         ("list_devices", {"include_inactive": True}), ("wan_details", {}), ("ethernet_ports", {}),
         ("wifi_details", {}), ("firewall_details", {}), ("dhcp_details", {}),
         # every tool call adds GUI login/logout events, so compare on wifi events only
         ("event_log", {"hours": 1000, "module": "WIFI", "limit": 30}),
         ("event_log", {"hours": 1000, "module": "WIFI", "device": "schuur", "limit": 20}),
         ("event_summary", {})]


def token() -> str:
    ini = configparser.ConfigParser()
    ini.read(REPO / "credentials.ini")
    return ini.get("mcp", "token")


def dump(model) -> dict:
    return model.model_dump(by_alias=True, exclude_none=True, mode="json")


def identity(value):
    """Keep only what should not change between two calls seconds apart."""
    if isinstance(value, dict):
        keep = {"hostname", "name", "mac", "device_id", "parent", "connected_via", "connection",
                "nickname", "parent_nickname", "connected_via_nickname", "model", "serial_number"}
        out = {"keys": sorted(value)}
        out.update({k: value[k] for k in sorted(value) if k in keep})
        out.update({k: identity(v) for k, v in value.items() if isinstance(v, (dict, list))})
        return out
    if isinstance(value, list):
        items = [identity(v) for v in value]
        return sorted(items, key=lambda v: json.dumps(v, sort_keys=True))  # order of equal-rank clients may differ
    return value


async def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", default="http://sagemcom-mcp.local/mcp")
    args = parser.parse_args()
    sagemcom5598_mcp._config.update(sagemcom5598_mcp._load_config(argparse.Namespace(db=":memory:", retention=1)))

    failures = 0
    http = create_mcp_http_client(headers={"Authorization": f"Bearer {token()}"})
    async with http, Client(streamable_http_client(args.url, http_client=http)) as board, \
            Client(sagemcom5598_mcp.mcp) as python:
        print(f"connected: protocol {board.protocol_version}, server {board.server_info.name if board.server_info else '?'}")

        board_tools = {t.name: dump(t) for t in (await board.list_tools()).tools}
        python_tools = {t.name: dump(t) for t in (await python.list_tools()).tools}
        for name, definition in board_tools.items():
            ok = definition == python_tools.get(name)
            failures += not ok
            print(f"tools/list {name}: {'same as Python' if ok else 'DIFFERENT'}")
            if not ok:
                print("   board :", json.dumps(definition))
                print("   python:", json.dumps(python_tools.get(name)))

        for name, arguments in CALLS:
            label = f"{name}({', '.join(f'{k}={v}' for k, v in arguments.items())})"
            b = await board.call_tool(name, arguments)
            p = await python.call_tool(name, arguments)
            if b.is_error or p.is_error:
                failures += 1
                print(f"{label}: ERROR board={b.content[0].text!r} python={p.content[0].text!r}")
                continue
            b_data, p_data = json.loads(b.content[0].text), json.loads(p.content[0].text)
            if name == "event_summary":
                # GUI logins (ours, the Python server's) keep changing the counts; "this MCP
                # server" marks the board's IP on one side and the laptop's on the other
                b_data = {"keys": sorted(b_data), "wifi_devices": b_data["wifi_devices"]}
                p_data = {"keys": sorted(p_data), "wifi_devices": p_data["wifi_devices"]}
                for d in b_data["wifi_devices"] + p_data["wifi_devices"]:
                    for k in ("connects", "disconnects", "auth_failures"):
                        d.pop(k)
            same = identity(b_data) == identity(p_data)
            failures += not same
            size = len(b.content[0].text)
            print(f"{label}: {'same structure and identities as Python' if same else 'DIFFERENT'} ({size} chars)")
            if not same:
                print("   board :", json.dumps(identity(b_data))[:600])
                print("   python:", json.dumps(identity(p_data))[:600])

        try:  # the spec wants a protocol error (-32602) for an unknown tool, which the SDK raises
            await board.call_tool("no_such_tool", {})
            failures += 1
            print("unknown tool: NO ERROR")
        except MCPError as exc:
            print(f"unknown tool: protocol error {exc.error.code} {exc.error.message!r}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    asyncio.run(main())
