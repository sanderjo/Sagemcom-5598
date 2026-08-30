"""Live integration tests. Requires a reachable Sagemcom 5598 and
credentials.ini (copy credentials.ini.example) in the repo root."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from sagemcom5598 import Sagemcom5598, _load_credentials_ini


class Sagemcom5598LiveTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        creds = _load_credentials_ini()
        if not creds.get("password"):
            raise unittest.SkipTest(
                "credentials.ini not found - copy credentials.ini.example "
                "and fill in your router password"
            )
        cls.client = Sagemcom5598()
        try:
            cls.client.login(
                ip=creds.get("ip", "192.168.1.254"),
                login=creds.get("login", "beheer"),
                password=creds["password"],
            )
        except Exception as exc:
            raise unittest.SkipTest(f"could not log in to router: {exc}")

    @classmethod
    def tearDownClass(cls):
        cls.client.logout()

    def test_connected_extenders(self):
        extenders = self.client.connected_extenders()
        self.assertIsInstance(extenders, list)
        for extender in extenders:
            for key in ("hostname", "model", "serial_number", "firmware", "ipv4"):
                self.assertIn(key, extender)

    def test_connected_devices(self):
        devices = self.client.connected_devices()
        self.assertIsInstance(devices, list)
        self.assertGreater(len(devices), 0)
        for device in devices:
            for key in ("name", "ip", "mac", "connection", "band", "signal_strength"):
                self.assertIn(key, device)
            self.assertIn(device["connection"], ("wired", "wireless"))

    def test_firewall_settings(self):
        firewall = self.client.firewall_settings()
        self.assertIsInstance(firewall, dict)
        for key in ("level", "custom_chain_enabled", "default_policy", "rules"):
            self.assertIn(key, firewall)
        self.assertIsInstance(firewall["rules"], list)
        for rule in firewall["rules"]:
            self.assertIn(rule["ip_version"], ("ipv4", "ipv6"))


if __name__ == "__main__":
    unittest.main()
