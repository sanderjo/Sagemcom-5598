#!/usr/bin/env python3
"""Save the router's raw replies for every endpoint the ESP32 tools use, as
fixtures for compare_tools.py. One login. The replies hold your network's
data (hostnames, MACs, addresses), so keep the output out of git.

usage: capture_fixtures.py OUTDIR
"""
import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))
from sagemcom5598 import Sagemcom5598, _load_credentials_ini  # noqa: E402

PATHS = [
    "/api/v1/open", "/api/v1/wan/ipv4", "/api/v1/wan/status", "/api/v1/wan/ip/stats", "/api/v1/lan/stats",
    "/api/v1/ntp", "/api/v2/home", "/api/v4/easymesh/meshdevices", "/api/v1/wireless/bandsteering",
    "/api/v2/wireless/mlo/state", "/api/v2/wireless/stats/24", "/api/v2/wireless/stats/5",
    "/api/v2/wireless/stats/6", "/api/v2/firewall", "/api/v2/firewall/chain?chain=Custom", "/api/v1/dhcp",
    "/api/v1/hosts",
]


def fixture_name(path: str) -> str:
    return path.strip("/").replace("/", "_").replace("?", "_").replace("=", "-") + ".json"


def main() -> None:
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    ini = _load_credentials_ini()
    client = Sagemcom5598()
    client.login(ip=ini.get("ip", "192.168.1.254"), login=ini.get("login", "beheer"), password=ini["password"])
    try:
        for path in PATHS:
            resp = client.session.get(client.base_url + path)
            resp.raise_for_status()
            (out / fixture_name(path)).write_text(resp.text)
            print(f"{path}: {len(resp.content)} bytes")
    finally:
        client.logout()


if __name__ == "__main__":
    main()
