#!/usr/bin/env python3
"""Client for the Sagemcom F@st 5598 (Delta Fiber) router web API."""

import argparse
import hashlib
import random

import requests

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
        return [
            {
                "hostname": dev.get("hostname"),
                "model": dev.get("model"),
                "serial_number": dev.get("serialNumber"),
                "firmware": dev.get("softwareVersion"),
                "ipv4": dev.get("ipv4"),
                "uptime": dev.get("upTime"),
                "backhaul": dev.get("backhaul"),
            }
            for dev in mesh.get("meshDevices", [])
            if dev.get("type") == "extender"
        ]

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


def _print_table(rows: list[dict], columns: list[str]) -> None:
    if not rows:
        print("(none)")
        return
    widths = {c: max(len(c), *(len(str(r.get(c, ""))) for r in rows)) for c in columns}
    print("  ".join(c.ljust(widths[c]) for c in columns))
    print("  ".join("-" * widths[c] for c in columns))
    for row in rows:
        print("  ".join(str(row.get(c, "")).ljust(widths[c]) for c in columns))


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


def _cli() -> None:
    parser = argparse.ArgumentParser(description="Sagemcom5598 router CLI")
    parser.add_argument("--login", dest="password", required=True, help="router admin password")
    parser.add_argument("--ip", default="192.168.1.254", help="router IP (default: 192.168.1.254)")
    parser.add_argument("--username", default="beheer", help="router login username (default: beheer)")
    parser.add_argument("--connected_extenders", action="store_true", help="show connected extenders")
    parser.add_argument("--connected_devices", action="store_true", help="show connected devices")
    parser.add_argument("--firewall_settings", action="store_true", help="show firewall settings")
    args = parser.parse_args()

    client = Sagemcom5598()
    try:
        client.login(ip=args.ip, login=args.username, password=args.password)
    except requests.exceptions.RequestException as exc:
        if isinstance(exc, (requests.exceptions.ConnectionError, requests.exceptions.Timeout)):
            reason = f"no router found on {args.ip}"
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
            _print_table(
                client.connected_extenders(),
                columns=["hostname", "model", "serial_number", "firmware", "ipv4"],
            )
            print()
        if args.connected_devices:
            print("Connected devices:")
            _print_table(
                client.connected_devices(),
                columns=["name", "ip", "mac", "connection", "band", "signal_strength", "connected_via"],
            )
            print()
        if args.firewall_settings:
            _print_firewall(client.firewall_settings())
    finally:
        client.logout()


if __name__ == "__main__":
    _cli()
