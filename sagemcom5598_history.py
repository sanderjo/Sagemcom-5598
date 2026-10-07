#!/usr/bin/env python3
"""Periodic snapshots of a Sagemcom 5598's state, kept in SQLite.

The router's own event log (see `Sagemcom5598.event_log()`) records
*events* - wifi (dis)connects, WAN drops, reboots - but not *measurements*:
signal strengths, throughput, extender uptimes and backhaul quality are only
ever available as "right now". This module samples those every few minutes
so questions like "was the extender's backhaul already bad last night?" or
"when did the WAN traffic stop?" can be answered afterwards.

Run standalone (cron/systemd) to collect around the clock:

    python3 sagemcom5598_history.py --interval 300

or let `sagemcom5598_mcp.py` collect in the background while it runs.
"""

import argparse
import json
import sqlite3
import sys
import time
from pathlib import Path

import requests

from sagemcom5598 import Sagemcom5598, _load_credentials_ini

DEFAULT_DB = Path(__file__).with_name("history.db")
DEFAULT_RETENTION_HOURS = 25


class HistoryStore:
    """Timestamped JSON snapshots in one SQLite table."""

    def __init__(self, path: Path | str = DEFAULT_DB) -> None:
        self.path = Path(path)
        with self._connect() as db:
            db.execute("CREATE TABLE IF NOT EXISTS snapshots (ts REAL PRIMARY KEY, data TEXT NOT NULL)")

    def _connect(self) -> sqlite3.Connection:
        return sqlite3.connect(self.path, timeout=10)

    def add(self, snapshot: dict, ts: float | None = None) -> None:
        with self._connect() as db:
            db.execute(
                "INSERT OR REPLACE INTO snapshots (ts, data) VALUES (?, ?)",
                (ts if ts is not None else time.time(), json.dumps(snapshot)),
            )

    def since(self, ts: float) -> list[tuple[float, dict]]:
        """(timestamp, snapshot) pairs newer than `ts`, oldest first."""
        with self._connect() as db:
            rows = db.execute("SELECT ts, data FROM snapshots WHERE ts >= ? ORDER BY ts", (ts,)).fetchall()
        return [(row_ts, json.loads(data)) for row_ts, data in rows]

    def prune(self, retention_hours: float = DEFAULT_RETENTION_HOURS) -> int:
        with self._connect() as db:
            cur = db.execute("DELETE FROM snapshots WHERE ts < ?", (time.time() - retention_hours * 3600,))
        return cur.rowcount


def take_snapshot(client: Sagemcom5598) -> dict:
    """Everything worth tracking over time, from one logged-in client."""
    return {
        "device": client.device_info(),
        "wan_ipv4": client.wan_ipv4(),
        "wan_stats": client.wan_stats(),
        "lan_ports": client.lan_ports(),
        "wifi_stats": client.wifi_stats(),
        "extenders": client.connected_extenders(),
        "devices": client.connected_devices(),
    }


def collect_once(store: HistoryStore, ip: str, login: str, password: str,
                 retention_hours: float = DEFAULT_RETENTION_HOURS) -> None:
    for attempt in range(2):  # the router occasionally drops a connection
        client = Sagemcom5598()
        try:
            client.login(ip=ip, login=login, password=password)
            try:
                snapshot = take_snapshot(client)
            finally:
                client.logout()
            break
        except requests.ConnectionError:
            if attempt:
                raise
            time.sleep(5)
    store.add(snapshot)
    store.prune(retention_hours)


def _cli() -> None:
    parser = argparse.ArgumentParser(description="Collect Sagemcom 5598 snapshots into SQLite")
    parser.add_argument("--db", default=str(DEFAULT_DB), help=f"SQLite file (default: {DEFAULT_DB.name} next to this script)")
    parser.add_argument("--interval", type=int, default=0, help="seconds between snapshots; 0 = take one and exit")
    parser.add_argument("--retention", type=float, default=DEFAULT_RETENTION_HOURS, help="hours of history to keep")
    parser.add_argument("--ip", default=None)
    parser.add_argument("--username", default=None)
    parser.add_argument("--login", dest="password", default=None, help="router password (default: credentials.ini)")
    args = parser.parse_args()

    ini = _load_credentials_ini()
    ip = args.ip or ini.get("ip") or "192.168.1.254"
    username = args.username or ini.get("login") or "beheer"
    password = args.password or ini.get("password")
    if not password:
        parser.error("password required: pass --login or provide credentials.ini")

    store = HistoryStore(args.db)
    while True:
        try:
            collect_once(store, ip, username, password, args.retention)
            print(f"{time.strftime('%Y-%m-%d %H:%M:%S')} snapshot stored", flush=True)
        except Exception as exc:  # keep collecting through router hiccups
            print(f"{time.strftime('%Y-%m-%d %H:%M:%S')} snapshot failed: {exc}", file=sys.stderr, flush=True)
            if not args.interval:
                raise SystemExit(1)
        if not args.interval:
            return
        time.sleep(args.interval)


if __name__ == "__main__":
    _cli()
