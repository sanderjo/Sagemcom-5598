#!/usr/bin/env python3
"""MCP server for the Sagemcom F@st 5598: lets an AI assistant inspect the
router's setup, read its event log and collected history, and diagnose
problems and their likely root causes.

Read-only by design - no tool changes router settings, and secrets the
router exposes (wifi passwords, PPP/PLOAM credentials) are never returned.

Configuration (environment variables override credentials.ini):
    SAGEMCOM_IP, SAGEMCOM_LOGIN, SAGEMCOM_PASSWORD, SAGEMCOM_HISTORY_DB

Run (stdio transport, as MCP clients like Claude Code expect):
    python3 sagemcom5598_mcp.py [--collect-interval 300] [--retention 25]
"""

import argparse
import functools
import os
import socket
import sys
import threading
import time
from contextlib import contextmanager

import requests
from mcp.server.mcpserver import MCPServer
from mcp.types import ToolAnnotations

import sagemcom5598_diagnose as diag
from sagemcom5598 import Sagemcom5598, _load_credentials_ini, add_nicknames, load_nicknames
from sagemcom5598_history import DEFAULT_DB, DEFAULT_RETENTION_HOURS, HistoryStore, collect_once

INSTRUCTIONS = """\
Tools for a Sagemcom F@st 5598 home gateway (Delta Fiber, NL) and its FAST381 mesh extenders.

Start troubleshooting with `diagnose` - it combines current state, the router's event log and
collected history into findings with evidence, likely cause and a suggested fix. Drill down with
`event_log` (filter by device/module), `event_summary`, `history_summary` and `device_history`.
For setup questions use `router_overview`, `network_topology`, `list_devices`, `wifi_details`,
`wan_details`, `ethernet_ports`, `firewall_details`, `dhcp_details`.

Notes: the router allows one admin session at a time, so each tool call logs in and out (a user
logged into the web GUI at the same time may be logged out). The router's event log holds about
a week of events; measurements (signal, throughput, uptimes) are only available as history when
the collector has been running. Signal strengths are in dBm: better than -65 good, -65..-75 fair,
below -75 weak. Times are in the router's local time zone. Nodes and clients that have a
friendly name in nicknames.txt carry it as `nickname` (or `parent_nickname`, `connected_via_nickname`,
`via_nickname`) next to the technical name; use it when talking to the user, and the
`device` arguments accept a nickname too."""

READ_ONLY = ToolAnnotations(readOnlyHint=True, idempotentHint=True, openWorldHint=False)

mcp = MCPServer("sagemcom5598", instructions=INSTRUCTIONS)


def _tool(func):
    """Register a read-only tool whose result gets nicknames added
    (nicknames.txt is re-read on every call, so edits apply right away)."""
    @functools.wraps(func)
    def wrapper(*args, **kwargs):
        return add_nicknames(func(*args, **kwargs), load_nicknames())
    return mcp.tool(annotations=READ_ONLY)(wrapper)


def _resolve_nickname(device: str) -> str:
    """The technical name (hostname/MAC) behind a nickname, else `device` itself."""
    by_nickname = {nick.lower(): name for name, nick in load_nicknames().items()}
    return by_nickname.get(device.lower(), device)
_lock = threading.Lock()
_config: dict = {}


def _load_config(args: argparse.Namespace) -> dict:
    ini = _load_credentials_ini()
    return {
        "ip": os.environ.get("SAGEMCOM_IP") or ini.get("ip") or "192.168.1.254",
        "login": os.environ.get("SAGEMCOM_LOGIN") or ini.get("login") or "beheer",
        "password": os.environ.get("SAGEMCOM_PASSWORD") or ini.get("password"),
        "db": os.environ.get("SAGEMCOM_HISTORY_DB") or args.db,
        "retention": args.retention,
    }


def _store() -> HistoryStore:
    return HistoryStore(_config["db"])


@contextmanager
def _router():
    """A logged-in client, serialized so we never hold two sessions."""
    if not _config.get("password"):
        raise RuntimeError("No router password configured: set SAGEMCOM_PASSWORD or create credentials.ini")
    with _lock:
        for attempt in range(2):  # the router occasionally drops a connection
            client = Sagemcom5598()
            try:
                client.login(ip=_config["ip"], login=_config["login"], password=_config["password"])
                break
            except (requests.ConnectionError, requests.Timeout):
                if attempt:
                    raise RuntimeError(f"No router reachable at {_config['ip']}")
                time.sleep(3)
            except requests.HTTPError as exc:
                if exc.response is not None and exc.response.status_code == 400:
                    raise RuntimeError("Router rejected the login (wrong password?)")
                raise
        try:
            yield client
        finally:
            try:
                client.logout()
            except requests.RequestException:
                pass


def _own_ip() -> str | None:
    """Our address as the router sees it, to tell our own GUI logins apart."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.connect((_config["ip"], 80))
            return sock.getsockname()[0]
    except OSError:
        return None


def _gather(client: Sagemcom5598) -> dict:
    return {
        "device": client.device_info(),
        "gateway": client.gateway_node(),
        "wan_ipv4": client.wan_ipv4(),
        "wan_status": client.wan_status(),
        "wan_stats": client.wan_stats(),
        "lan_ports": client.lan_ports(),
        "wifi_stats": client.wifi_stats(),
        "extenders": client.connected_extenders(),
        "devices": client.connected_devices(),
        "hosts": client.hosts(),
        "ntp": client.ntp(),
    }


def _history(hours: float) -> list[tuple[float, dict]]:
    return _store().since(time.time() - hours * 3600)


@_tool
def diagnose(hours: float = 25) -> dict:
    """Check the whole setup for problems over the last `hours` (default 25):
    WAN drops, reboots of gateway/extenders, flapping or failing wifi clients,
    weak signal and weak mesh backhaul, slow/erroring Ethernet links, clock
    sync, plus changes seen in collected history. Returns findings sorted by
    severity, each with evidence, likely root cause and suggested fix."""
    with _router() as client:
        state = _gather(client)
        events = client.event_log()
    return diag.diagnose(state, events, hours, _history(hours), _own_ip())


@_tool
def router_overview() -> dict:
    """Identity and health at a glance: model, serial, firmware, uptime,
    WAN status/IP, clock sync, number of extenders and clients."""
    with _router() as client:
        device = client.device_info()
        gateway = client.gateway_node()
        extenders = client.connected_extenders()
        devices = client.connected_devices()
        return {
            "model": gateway["model"],
            "hostname": gateway["hostname"],
            "serial_number": device["serial_number"],
            "firmware": device["firmware"],
            "uptime_seconds": device["uptime"],
            "lan_ip": gateway["ipv4"],
            "wan": client.wan_ipv4(),
            "ntp": client.ntp(),
            "extenders": [{k: e[k] for k in ("hostname", "model", "firmware", "ipv4", "parent")} for e in extenders],
            "clients": {
                "wired": sum(1 for d in devices if d["connection"] == "wired"),
                "wireless": sum(1 for d in devices if d["connection"] == "wireless"),
            },
        }


@_tool
def network_topology() -> dict:
    """The mesh as a tree: gateway at the root, extenders nested under the
    node they backhaul through, each node with its directly connected
    clients (band, signal strength, link speed)."""
    with _router() as client:
        return client.topology()


@_tool
def list_devices(include_inactive: bool = False) -> dict:
    """Connected clients (wired and wireless) with IP, MAC, band, signal and
    the mesh node they're on. With `include_inactive`, also devices the
    router knows but that are currently offline (with last-seen time)."""
    with _router() as client:
        devices = client.connected_devices()
        if include_inactive:
            connected = {d["mac"].lower() for d in devices}
            devices += [
                {**h, "connection": "offline"}
                for h in client.hosts()
                if not h["active"] and h["mac"].lower() not in connected
            ]
        return {"count": len(devices), "devices": devices}


@_tool
def list_extenders() -> dict:
    """Mesh extenders: model, firmware, uptime, parent node, and backhaul
    (Ethernet speed, or per-band wifi signal/quality/channel)."""
    with _router() as client:
        return {"extenders": client.connected_extenders()}


@_tool
def wan_details() -> dict:
    """Internet uplink: status, time since last change, public IPv4
    (flags CGNAT), gateway, WAN port speed and traffic/error counters."""
    with _router() as client:
        ipv4 = client.wan_ipv4()
        wan_port = next((p for p in client.lan_ports() if p["role"] == "WAN"), None)
        return {
            "ipv4": ipv4,
            "cgnat": ipv4["address"].startswith("100.") and 64 <= int(ipv4["address"].split(".")[1]) <= 127,
            "status": client.wan_status(),
            "traffic": client.wan_stats(),
            "port": wan_port,
        }


@_tool
def ethernet_ports() -> dict:
    """The gateway's physical Ethernet ports: link status, negotiated speed
    and duplex, rx/tx counters including errors and discards."""
    with _router() as client:
        return {"ports": client.lan_ports()}


@_tool
def wifi_details() -> dict:
    """Wifi setup: SSIDs per band (status, security; no passwords), channel
    per band per mesh node, band steering and MLO switches, and the
    gateway's per-band traffic/error counters."""
    with _router() as client:
        return {**client.wifi_config(), "gateway_radio_stats": client.wifi_stats()}


@_tool
def firewall_details() -> dict:
    """Firewall level, port-scan/fragment protection, and the custom rule
    chain (per rule: action, IPv4/IPv6, protocol, ports, direction)."""
    with _router() as client:
        return client.firewall_settings()


@_tool
def dhcp_details() -> dict:
    """LAN DHCP server config (pool, lease time, router IP) and every host
    the router knows with its IPv4/IPv6 addresses and lease state."""
    with _router() as client:
        return {"server": client.dhcp(), "hosts": client.hosts()}


@_tool
def event_log(hours: float = 25, module: str | None = None, level: str | None = None,
              device: str | None = None, contains: str | None = None, limit: int = 200) -> dict:
    """The router's own event log for the last `hours`, newest first.
    Filters: `module` (WIFI, SYS, GUI, DNS, DHCPC, DHCPS, WETH=WAN Ethernet,
    LETH=LAN Ethernet), `level` (info, warning, err), `device` (MAC or part
    of a hostname), `contains` (text). MACs are annotated with device names."""
    with _router() as client:
        events = client.event_log()
        state = {"hosts": client.hosts(), "extenders": client.connected_extenders(),
                 "gateway": client.gateway_node(), "devices": client.connected_devices()}
    names = diag.names_from_state(state)
    selected = diag.events_in_window(events, hours)
    if module:
        selected = [e for e in selected if e["module"].lower() == module.lower()]
    if level:
        selected = [e for e in selected if e["level"].lower().startswith(level.lower()[:3])]
    if contains:
        selected = [e for e in selected if contains.lower() in e["message"].lower()]
    if device:
        # match on the resolved name too: extenders log with their radio MACs, not their deviceId
        needle = _resolve_nickname(device).lower()

        def matches(event: dict) -> bool:
            mac = diag.classify(event)[1].get("mac")
            return bool(mac) and (needle in mac.lower() or needle in (names.resolve(mac)["name"] or "").lower())

        selected = [e for e in selected if matches(e)]

    out = []
    for event in reversed(selected[-limit:] if limit else selected):
        kind, groups = diag.classify(event)
        entry = {**event, "kind": kind}
        if "mac" in groups:
            entry["device"] = names.resolve(groups["mac"])
        out.append(entry)
    return {"matched": len(selected), "returned": len(out), "events": out}


@_tool
def event_summary(hours: float = 25) -> dict:
    """The event log over the last `hours` condensed: counts per event kind,
    per wifi device (connects, disconnects, failed logins, SSIDs), GUI admin
    logins by source IP, and any uncategorized events."""
    with _router() as client:
        events = client.event_log()
        state = {"hosts": client.hosts(), "extenders": client.connected_extenders(),
                 "gateway": client.gateway_node(), "devices": client.connected_devices()}
    return diag.summarize_events(events, diag.names_from_state(state), hours, _own_ip())


@_tool
def history_summary(hours: float = 25) -> dict:
    """What changed over the last `hours` according to the periodic
    snapshots: reboots, firmware/WAN IP/mesh parent/backhaul/port changes,
    hourly WAN throughput and client counts, extender backhaul signal
    ranges, and per-client presence, signal range and roaming. Only covers
    periods when the collector was running."""
    return diag.summarize_history(_history(hours))


@_tool
def device_history(device: str, hours: float = 25) -> dict:
    """Timeline for one client or extender (MAC or part of its name) from
    the snapshots: per snapshot the node/band/signal it had, or its
    backhaul signals for an extender; `absent` where it wasn't connected."""
    needle = _resolve_nickname(device).lower()
    timeline = []
    for ts, snap in _history(hours):
        when = time.strftime("%Y-%m-%d %H:%M", time.localtime(ts))
        match = next((d for d in snap.get("devices", [])
                      if needle in (d.get("mac") or "").lower() or needle in (d.get("name") or "").lower()), None)
        if match:
            timeline.append({"time": when, **{k: match.get(k) for k in
                            ("name", "connection", "band", "signal_strength", "link_quality", "connected_via", "link_speed_mbps")}})
            continue
        ext = next((e for e in snap.get("extenders", [])
                    if needle in (e.get("hostname") or "").lower() or needle in (e.get("device_id") or "").lower()), None)
        if ext:
            timeline.append({"time": when, "hostname": ext["hostname"], "uptime": ext.get("uptime"),
                             "parent": ext.get("parent"), "firmware": ext.get("firmware"),
                             "backhaul": ext.get("backhaul")})
            continue
        timeline.append({"time": when, "absent": True})
    return {"device": device, "snapshots": len(timeline), "timeline": timeline}


def _collector(interval: int) -> None:
    store = _store()
    while True:
        try:
            with _lock:
                collect_once(store, _config["ip"], _config["login"], _config["password"], _config["retention"])
        except Exception as exc:  # the router may be down - that's what the gap will show
            print(f"history snapshot failed: {exc}", file=sys.stderr, flush=True)
        time.sleep(interval)


def main() -> None:
    parser = argparse.ArgumentParser(description="MCP server for the Sagemcom 5598")
    parser.add_argument("--collect-interval", type=int, default=300,
                        help="seconds between history snapshots while running; 0 disables (default 300)")
    parser.add_argument("--retention", type=float, default=DEFAULT_RETENTION_HOURS, help="hours of history to keep")
    parser.add_argument("--db", default=str(DEFAULT_DB), help="SQLite history file")
    args = parser.parse_args()
    _config.update(_load_config(args))

    if args.collect_interval > 0 and _config["password"]:
        threading.Thread(target=_collector, args=(args.collect_interval,), daemon=True).start()
    mcp.run()


if __name__ == "__main__":
    main()
