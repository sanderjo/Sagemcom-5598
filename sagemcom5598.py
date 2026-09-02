#!/usr/bin/env python3
"""Client for the Sagemcom F@st 5598 (Delta Fiber) router web API."""

import argparse
import configparser
import hashlib
import random
from pathlib import Path

import requests

_CREDENTIALS_INI = Path(__file__).with_name("credentials.ini")

_B64_ALPHABET = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"

# Byte read-order used by the router's own JS when base64-encoding the final
# sha512-crypt digest. Not the natural 0..63 order - reverse engineered from
# the minified app bundle (main.*.js, function `Dt`).
_DIGEST_BYTE_ORDER = [
    42, 21, 0, 1, 43, 22, 23, 2, 44, 45, 24, 3, 4, 46, 25, 26, 5, 47, 48, 27,
    6, 7, 49, 28, 29, 8, 50, 51, 30, 9, 10, 52, 31, 32, 11, 53, 54, 33, 12,
    13, 55, 34, 35, 14, 56, 57, 36, 15, 16, 58, 37, 38, 17, 59, 60, 39, 18,
    19, 61, 40, 41, 20, 62, 63,
]


def _sequence(digest: bytes, length: int) -> bytes:
    full, remainder = divmod(length, len(digest))
    return digest * full + digest[:remainder]


def _encode_digest(digest: bytes) -> str:
    chars = []
    order = _DIGEST_BYTE_ORDER
    for i in range(0, len(order), 3):
        b0 = digest[order[i]]
        if i + 1 >= len(order):
            chars.append(_B64_ALPHABET[b0 & 0x3F])
            chars.append(_B64_ALPHABET[(b0 & 0xC0) >> 6])
        else:
            b1 = digest[order[i + 1]]
            b2 = digest[order[i + 2]]
            chars.append(_B64_ALPHABET[b0 & 0x3F])
            chars.append(_B64_ALPHABET[((b0 & 0xC0) >> 6) | ((b1 & 0x0F) << 2)])
            chars.append(_B64_ALPHABET[((b1 & 0xF0) >> 4) | ((b2 & 0x03) << 4)])
            chars.append(_B64_ALPHABET[(b2 & 0xFC) >> 2])
    return "".join(chars)


def _sha512_crypt(password: bytes, salt: str, rounds: int = 5000) -> str:
    """SHA-512-crypt ($6$), reimplemented from the router's own JS so it can
    be computed without relying on the (Unix-only) stdlib `crypt` module."""
    salt_b = salt.encode("latin-1")
    if not 8 <= len(salt_b) <= 16:
        raise ValueError(f"salt must be 8-16 bytes, got {len(salt_b)}")

    digest_b = hashlib.sha512(password + salt_b + password).digest()
    seq = password + salt_b + _sequence(digest_b, len(password))
    n = len(password)
    while n > 0:
        seq += digest_b if (n & 1) else password
        n >>= 1
    digest_a = hashlib.sha512(seq).digest()

    seq_p = _sequence(hashlib.sha512(password * len(password)).digest(), len(password))
    seq_s = _sequence(
        hashlib.sha512(salt_b * (16 + digest_a[0])).digest(), len(salt_b)
    )

    digest = digest_a
    for round_ in range(rounds):
        mixed = seq_p if (round_ & 1) else digest
        if round_ % 3:
            mixed += seq_s
        if round_ % 7:
            mixed += seq_p
        mixed += digest if (round_ & 1) else seq_p
        digest = hashlib.sha512(mixed).digest()

    return f"{salt}${_encode_digest(digest)}"


def _sha512_hex(text: str) -> str:
    return hashlib.sha512(text.encode("utf-8")).hexdigest()


class Sagemcom5598:
    """Get/set items on a Sagemcom F@st 5598 router over its REST/JSON API."""

    def __init__(self) -> None:
        self.base_url = ""
        self.username = ""
        self.session = requests.Session()
        self.session.headers.update({"Accept": "application/json, text/plain, */*"})

    def login(
        self,
        ip: str = "192.168.1.254",
        login: str = "beheer",
        password: str | None = None,
    ) -> None:
        if password is None:
            raise ValueError("password is required")

        self.base_url = f"http://{ip}"
        self.username = login
        self.session.headers.update(
            {"Referer": f"{self.base_url}/", "X-CSRF-Token": ""}
        )

        self.session.get(f"{self.base_url}/api/v1/open")

        resp = self.session.post(
            f"{self.base_url}/api/v2/login-params", data={"login": login}
        )
        resp.raise_for_status()
        params = resp.json()[0]
        salt, nonce = params["salt"], params["nonce"]

        crypt_hash = _sha512_crypt(password.encode("utf-8"), salt)
        step2 = _sha512_hex(f"{login}:{nonce}:{crypt_hash}")
        cnonce = str(random.randint(0, 10**19 - 1)).zfill(19)
        auth_key = _sha512_hex(f"{step2}:0:{cnonce}")

        resp = self.session.post(
            f"{self.base_url}/api/v1/login",
            data={"login": login, "auth_key": auth_key, "cnonce": cnonce},
        )
        resp.raise_for_status()

    def logout(self) -> None:
        self.session.post(f"{self.base_url}/api/v1/logout")

    def _get_json(self, path: str, params: dict | None = None):
        resp = self.session.get(f"{self.base_url}{path}", params=params)
        resp.raise_for_status()
        return resp.json()

    def connected_extenders(self) -> list[dict]:
        mesh = self._get_json("/api/v4/easymesh/meshdevices")[0]
        devices = mesh.get("meshDevices", [])
        hostname_by_device_id = {dev.get("deviceId"): dev.get("hostname") for dev in devices}

        extenders = []
        for dev in devices:
            if dev.get("type") != "extender":
                continue
            backhaul = dev.get("backhaul") or {}
            extenders.append(
                {
                    "hostname": dev.get("hostname"),
                    "model": dev.get("model"),
                    "serial_number": dev.get("serialNumber"),
                    "firmware": dev.get("softwareVersion"),
                    "ipv4": dev.get("ipv4"),
                    "uptime": dev.get("upTime"),
                    "parent": hostname_by_device_id.get(backhaul.get("rootDeviceId")),
                    "backhaul": backhaul,
                    "signal_strength_dbm": {
                        link["band"]: link.get("signalStrength")
                        for link in backhaul.get("wifiLinks", [])
                    },
                }
            )
        return extenders

    def connected_devices(self) -> list[dict]:
        mesh = self._get_json("/api/v4/easymesh/meshdevices")[0]
        devices = []
        for dev in mesh.get("meshDevices", []):
            for radio in dev.get("wifiRadios", []):
                band = radio.get("band")
                for ssid in radio.get("ssids", []):
                    for station in ssid.get("stations", []):
                        devices.append(
                            {
                                "name": station.get("hostName")
                                or station.get("friendlyname")
                                or station.get("macAddress"),
                                "ip": station.get("ipv4Address"),
                                "mac": station.get("macAddress"),
                                "connection": "wireless",
                                "band": band,
                                "signal_strength": station.get("signalStrength"),
                                "ssid": ssid.get("ssid"),
                                "link_quality": station.get("linkQuality"),
                                "connected_via": dev.get("hostname"),
                            }
                        )
            for port in dev.get("ethernetPorts", []):
                for neighbour in port.get("neighbours", []):
                    devices.append(
                        {
                            "name": neighbour.get("hostName")
                            or neighbour.get("friendlyname")
                            or neighbour.get("macAddress"),
                            "ip": neighbour.get("ipv4Address"),
                            "mac": neighbour.get("macAddress"),
                            "connection": "wired",
                            "band": None,
                            "signal_strength": None,
                            "ssid": None,
                            "link_quality": None,
                            "connected_via": dev.get("hostname"),
                            "link_speed_mbps": port.get("speed"),
                        }
                    )
        return devices

    def topology(self) -> dict:
        """The mesh as a tree: the gateway at the root, each extender
        nested under whatever it actually backhauls through (gateway or,
        in a multi-hop mesh, another extender - see `parent` in
        `connected_extenders()`), with every node's directly-connected
        clients attached to it. Mirrors the picture at
        #/wifi/2.4GHz/priv/mesh/overview."""
        mesh = self._get_json("/api/v4/easymesh/meshdevices")[0]
        gateway = next(dev for dev in mesh.get("meshDevices", []) if dev.get("type") == "gateway")

        extenders_by_parent: dict[str, list[dict]] = {}
        for extender in self.connected_extenders():
            extenders_by_parent.setdefault(extender["parent"], []).append(extender)

        clients_by_via: dict[str, list[dict]] = {}
        for client in self.connected_devices():
            clients_by_via.setdefault(client["connected_via"], []).append(client)
        for clients in clients_by_via.values():
            clients.sort(key=lambda c: c["connection"] != "wired")

        def build(hostname: str, ipv4: str, signal_strength_dbm: dict | None) -> dict:
            return {
                "hostname": hostname,
                "ipv4": ipv4,
                "signal_strength_dbm": signal_strength_dbm,
                "clients": clients_by_via.get(hostname, []),
                "extenders": [
                    build(ext["hostname"], ext["ipv4"], ext["signal_strength_dbm"])
                    for ext in extenders_by_parent.get(hostname, [])
                ],
            }

        return build(gateway.get("hostname"), gateway.get("ipv4"), None)

    def firewall_settings(self) -> dict:
        firewall = self._get_json("/api/v2/firewall")[0]["firewall"]
        custom_chain = self._get_json("/api/v2/firewall/chain", params={"chain": "Custom"})[0]

        rules = [
            {
                "id": rule.get("id"),
                "alias": rule.get("alias"),
                "description": rule.get("description"),
                "enabled": rule.get("enable"),
                "action": rule.get("action"),
                "ip_version": "ipv6" if rule.get("ip_protocol") == "ipv6" else "ipv4",
                "protocol": rule.get("protocol"),
                "src_ip": rule.get("src_ip"),
                "src_ports": rule.get("src_ports"),
                "src_interface": rule.get("src_intf"),
                "dst_ip": rule.get("dst_ip"),
                "dst_ports": rule.get("dst_ports"),
                "dst_interface": rule.get("dst_intf"),
            }
            for rule in custom_chain.get("rules", [])
        ]

        return {
            "level": firewall.get("level"),
            "port_scan_detection": firewall.get("port_scan_detection"),
            "block_fragmented_ip_packets": firewall.get("block_fragmented_ip_packets"),
            "custom_chain_enabled": custom_chain.get("enable"),
            "default_policy": custom_chain.get("default_policy"),
            "rules": rules,
        }

    def firewall_allow_ipv6_port(self, port: int) -> None:
        """Add a pair of Custom-chain firewall rules that Accept ipv6
        tcp+udp traffic on `port` - one per direction (lan->wan, wan->lan) -
        replicating the two requests the web UI's "allow port" quick action
        sends (see 192.168.1.254-new-login-and-firewall-ipv6-allow-port-5076.har)."""
        description = f"ipv6 port {port} "
        for src_intf, dst_intf, src_ports, dst_ports in (
            ("lan", "wan", port, -1),
            ("wan", "lan", -1, port),
        ):
            resp = self.session.post(
                f"{self.base_url}/api/v2/firewall/chain/rules",
                data={
                    "chain": "Custom",
                    "enable": 1,
                    "action": "Accept",
                    "description": description,
                    "service": "NONE",
                    "dst_ports": dst_ports,
                    "protocol": "tcp,udp",
                    "src_ports": src_ports,
                    "order": 6,
                    "src_intf": src_intf,
                    "dst_intf": dst_intf,
                    "ip_protocol": "ipv6",
                    "dst_port_range_max": -1,
                    "src_port_range_max": -1,
                },
            )
            resp.raise_for_status()

    def firewall_remove_ipv6_port(self, port: int) -> int:
        """Remove every Custom-chain ipv6 rule that allows `port` (as
        src_ports or dst_ports), i.e. the rule pair added by
        `firewall_allow_ipv6_port()`. Returns the number of rules removed.
        See 192.168.1.254-new-login-and-firewall-ipv6-remove-port-5076.har."""
        custom_chain = self._get_json("/api/v2/firewall/chain", params={"chain": "Custom"})[0]
        matching_ids = [
            rule["id"]
            for rule in custom_chain.get("rules", [])
            if rule.get("ip_protocol") == "ipv6"
            and port in (rule.get("src_ports"), rule.get("dst_ports"))
        ]
        for rule_id in matching_ids:
            resp = self.session.delete(
                f"{self.base_url}/api/v2/firewall/chain/rules/{rule_id}",
                data={"chain": "Custom"},
            )
            resp.raise_for_status()
        return len(matching_ids)

    def wifi_stats(self) -> dict:
        bands = {"2.4": "24", "5": "5", "6": "6"}
        stats = {}
        for label, path_band in bands.items():
            ssid = self._get_json(f"/api/v2/wireless/stats/{path_band}")[0]["wireless"]["ssid"]
            stats[label] = {
                "status": ssid.get("status"),
                "max_bitrate_mbps": ssid.get("maxbitrate"),
                "rx": ssid.get("stats", {}).get("rx"),
                "tx": ssid.get("stats", {}).get("tx"),
            }
        return stats

    def wan_stats(self) -> dict:
        """Total bytes in/out on the WAN link - unlike wifi_stats(), this
        includes traffic from every client, wired or wireless, on the
        gateway or on any extender, since it's downstream of all of them."""
        stats = self._get_json("/api/v1/wan/ip/stats")[0]["wan"]["ip"]["stats"]
        return {
            "rx": {k: int(v) for k, v in stats["rx"].items()},
            "tx": {k: int(v) for k, v in stats["tx"].items()},
        }


def _print_table(rows: list[dict], columns: list[str]) -> None:
    if not rows:
        print("(none)")
        return
    widths = {c: max(len(c), *(len(str(r.get(c, ""))) for r in rows)) for c in columns}
    print("  ".join(c.ljust(widths[c]) for c in columns))
    print("  ".join("-" * widths[c] for c in columns))
    for row in rows:
        print("  ".join(str(row.get(c, "")).ljust(widths[c]) for c in columns))


def _format_client_line(client: dict) -> str:
    name = (client["name"] or "").ljust(26)
    ip = (client["ip"] or "").ljust(15)
    if client["connection"] == "wired":
        return f"{name} {ip} wired"
    return f"{name} {ip} wireless {client['band']}GHz  {client['signal_strength']} dBm"


def _format_extender_line(node: dict) -> str:
    name = node["hostname"].ljust(26)
    ip = (node["ipv4"] or "").ljust(15)
    backhaul = "  ".join(
        f"{band}={node['signal_strength_dbm'].get(band)}" for band in ("2.4", "5", "6")
    )
    return f"{name} {ip} extender  backhaul {backhaul} dBm"


def _topology_lines(node: dict, indent: str) -> list[str]:
    clients = node["clients"]
    extenders = node["extenders"]
    if not clients and not extenders:
        return [f"{indent}(no clients)"]

    lines = []
    if clients:
        lines.append(f"{indent}|")
        lines += [f"{indent}+-- {_format_client_line(c)}" for c in clients]
    if extenders:
        lines.append(f"{indent}|")
        for extender in extenders:
            lines.append(f"{indent}+-- {_format_extender_line(extender)}")
            lines += _topology_lines(extender, indent + "    ")
    return lines


def _print_topology(tree: dict) -> None:
    print(tree["hostname"])
    for line in _topology_lines(tree, ""):
        print(line)


def _print_firewall(fw: dict) -> None:
    print(f"level: {fw['level']}")
    print(f"port_scan_detection: {fw['port_scan_detection']}")
    print(f"block_fragmented_ip_packets: {fw['block_fragmented_ip_packets']}")
    print(f"custom_chain_enabled: {fw['custom_chain_enabled']}")
    print(f"default_policy: {fw['default_policy']}")
    print()
    print("Custom rules:")
    _print_table(
        fw["rules"],
        columns=[
            "id", "alias", "action", "ip_version", "protocol",
            "src_interface", "src_ports", "dst_interface", "dst_ports",
            "description",
        ],
    )


def _mb(num_bytes: int) -> str:
    return f"{num_bytes / (1024 * 1024):.2f}"


def _format_uptime(seconds: str | int) -> str:
    total = int(seconds)
    days, rem = divmod(total, 86400)
    hours, rem = divmod(rem, 3600)
    minutes, secs = divmod(rem, 60)
    if days:
        return f"{days}d {hours:02d}:{minutes:02d}:{secs:02d}"
    return f"{hours:02d}:{minutes:02d}:{secs:02d}"


def _print_wifi_stats(stats: dict) -> None:
    rows = [
        {
            "band": band,
            "status": data["status"],
            "max_bitrate_mbps": data["max_bitrate_mbps"],
            "rx_Mbytes": _mb(data["rx"]["bytes"]),
            "rx_packets": data["rx"]["packets"],
            "rx_errors": data["rx"]["packetserrors"],
            "tx_Mbytes": _mb(data["tx"]["bytes"]),
            "tx_packets": data["tx"]["packets"],
            "tx_errors": data["tx"]["packetserrors"],
        }
        for band, data in stats.items()
    ]
    _print_table(
        rows,
        columns=[
            "band", "status", "max_bitrate_mbps",
            "rx_Mbytes", "rx_packets", "rx_errors",
            "tx_Mbytes", "tx_packets", "tx_errors",
        ],
    )


def _print_wan_stats(stats: dict) -> None:
    row = {
        "interface": "wan",
        "rx_Mbytes": _mb(stats["rx"]["bytes"]),
        "rx_packets": stats["rx"]["packets"],
        "rx_errors": stats["rx"]["packetserrors"],
        "tx_Mbytes": _mb(stats["tx"]["bytes"]),
        "tx_packets": stats["tx"]["packets"],
        "tx_errors": stats["tx"]["packetserrors"],
    }
    _print_table(
        [row],
        columns=["interface", "rx_Mbytes", "rx_packets", "rx_errors", "tx_Mbytes", "tx_packets", "tx_errors"],
    )


def _load_credentials_ini() -> dict:
    if not _CREDENTIALS_INI.is_file():
        return {}
    config = configparser.ConfigParser()
    config.read(_CREDENTIALS_INI)
    if not config.has_section("router"):
        return {}
    return dict(config["router"])


def _cli() -> None:
    parser = argparse.ArgumentParser(description="Sagemcom5598 router CLI")
    parser.add_argument(
        "--login", dest="password", default=None,
        help="router admin password (falls back to credentials.ini if omitted)",
    )
    parser.add_argument("--ip", default=None, help="router IP (default: 192.168.1.254, or credentials.ini)")
    parser.add_argument("--username", default=None, help="router login username (default: beheer, or credentials.ini)")
    parser.add_argument("--connected_extenders", action="store_true", help="show connected extenders")
    parser.add_argument("--connected_devices", action="store_true", help="show connected devices")
    parser.add_argument("--topology", action="store_true", help="show mesh topology as ASCII art")
    parser.add_argument("--firewall_settings", action="store_true", help="show firewall settings")
    parser.add_argument(
        "--firewall_allow_ipv6_port", type=int, default=None, metavar="PORT",
        help="allow ipv6 tcp/udp traffic on PORT (adds Custom firewall rules, both directions)",
    )
    parser.add_argument(
        "--firewall_remove_ipv6_port", type=int, default=None, metavar="PORT",
        help="remove ipv6 firewall rules allowing PORT (undoes --firewall_allow_ipv6_port)",
    )
    parser.add_argument("--wifi_stats", action="store_true", help="show wifi stats for 2.4/5/6 GHz bands")
    parser.add_argument("--wan_stats", action="store_true", help="show total WAN rx/tx bytes")
    args = parser.parse_args()

    ini = _load_credentials_ini()
    ip = args.ip or ini.get("ip") or "192.168.1.254"
    username = args.username or ini.get("login") or "beheer"
    password = args.password or ini.get("password")
    if not password:
        parser.error("password required: pass --login or provide credentials.ini")

    client = Sagemcom5598()
    try:
        client.login(ip=ip, login=username, password=password)
    except requests.exceptions.RequestException as exc:
        if isinstance(exc, (requests.exceptions.ConnectionError, requests.exceptions.Timeout)):
            reason = f"no router found on {ip}"
        elif isinstance(exc, requests.exceptions.HTTPError) and exc.response is not None and exc.response.status_code == 400:
            reason = "password is incorrect"
        else:
            reason = str(exc)
        print(f"Login failed: {reason}")
        raise SystemExit(1)

    print("Login OK")

    try:
        if args.connected_extenders:
            print("Connected extenders:")
            extenders = client.connected_extenders()
            for extender in extenders:
                signal = extender["signal_strength_dbm"]
                extender["signal_2.4ghz_dbm"] = signal.get("2.4")
                extender["signal_5ghz_dbm"] = signal.get("5")
                extender["signal_6ghz_dbm"] = signal.get("6")
                extender["uptime"] = _format_uptime(extender["uptime"])
            _print_table(
                extenders,
                columns=[
                    "hostname", "model", "serial_number", "firmware", "ipv4", "uptime", "parent",
                    "signal_2.4ghz_dbm", "signal_5ghz_dbm", "signal_6ghz_dbm",
                ],
            )
            print()
        if args.connected_devices:
            print("Connected devices:")
            _print_table(
                client.connected_devices(),
                columns=["name", "ip", "mac", "connection", "band", "signal_strength", "connected_via"],
            )
            print()
        if args.topology:
            _print_topology(client.topology())
            print()
        if args.firewall_settings:
            _print_firewall(client.firewall_settings())
        if args.firewall_allow_ipv6_port:
            client.firewall_allow_ipv6_port(args.firewall_allow_ipv6_port)
            print(f"Allowed ipv6 port {args.firewall_allow_ipv6_port}")
        if args.firewall_remove_ipv6_port:
            removed = client.firewall_remove_ipv6_port(args.firewall_remove_ipv6_port)
            print(f"Removed {removed} ipv6 firewall rule(s) for port {args.firewall_remove_ipv6_port}")
        if args.wifi_stats:
            print("Wifi stats:")
            _print_wifi_stats(client.wifi_stats())
            print()
        if args.wan_stats:
            print("Wan stats:")
            _print_wan_stats(client.wan_stats())
    finally:
        client.logout()


if __name__ == "__main__":
    _cli()
