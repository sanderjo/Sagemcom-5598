"""Offline tests for sagemcom5598_diagnose - no router needed."""

import sys
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import sagemcom5598_diagnose as diag

NOW = datetime.now(timezone.utc)


def event(minutes_ago: float, message: str, module: str = "WIFI", level: str = "info") -> dict:
    return {"time": (NOW - timedelta(minutes=minutes_ago)).isoformat(), "level": level,
            "module": module, "message": message, "clock_corrected": False}


def base_state(**overrides) -> dict:
    state = {
        "device": {"uptime": 10 * 86400, "firmware": "FW1"},
        "gateway": {"hostname": "mygateway", "device_id": "ec:fc:2f:47:e7:74"},
        "wan_ipv4": {"status": "Up", "address": "84.1.2.3"},
        "wan_status": {"status": "Up", "last_change": 10 * 86400},
        "wan_stats": {"rx": {"bytes": 0}, "tx": {"bytes": 0}},
        "lan_ports": [],
        "wifi_stats": {},
        "extenders": [{
            "hostname": "ext1", "device_id": "ec:fc:2f:5b:f2:b0", "uptime": str(10 * 86400),
            "firmware": "E1", "parent": "mygateway",
            "backhaul": {"linkType": "Wi-Fi", "wifiLinks": [
                {"band": "5", "signalStrength": -55, "linkQuality": "GOOD"}]},
        }],
        "devices": [{"name": "phone", "mac": "44:07:0b:5f:8b:7c", "connection": "wireless",
                     "band": "2.4", "signal_strength": -60, "connected_via": "mygateway"}],
        "hosts": [{"name": "phone", "mac": "44:07:0b:5f:8b:7c"}],
        "ntp": {"status": "SYNCHRONIZED"},
    }
    state.update(overrides)
    return state


def titles(result: dict) -> list[str]:
    return [f["title"] for f in result["findings"]]


class ClassifyTest(unittest.TestCase):
    def test_wifi_connect(self):
        kind, groups = diag.classify(event(0, "A WiFi device <AA:BB:CC:DD:EE:FF> has successfully "
                                              "connected to SSID (Device/WiFi/SSIDs/SSID[WLAN_2_4])"))
        self.assertEqual(kind, "wifi_connect")
        self.assertEqual(groups, {"mac": "aa:bb:cc:dd:ee:ff", "ssid": "WLAN_2_4"})

    def test_wan_casing(self):
        self.assertEqual(diag.classify(event(0, "Wan Ethernet connectivity has been disconnected"))[0], "wan_down")
        self.assertEqual(diag.classify(event(0, "WAN Ethernet connectivity has been established"))[0], "wan_up")

    def test_ssid_band(self):
        self.assertEqual(diag.ssid_band("WLAN_2_4"), "2.4")
        self.assertEqual(diag.ssid_band("WL_BACKHAUL_6"), "6")

    def test_private_mac(self):
        self.assertTrue(diag.is_private_mac("de:f6:91:3b:26:8e"))
        self.assertFalse(diag.is_private_mac("44:07:0b:5f:8b:7c"))


class NamesTest(unittest.TestCase):
    def test_mesh_radio_mac_maps_to_nearest_node(self):
        names = diag.Names([], [{"hostname": "ext-a", "device_id": "ec:fc:2f:5b:f2:90"},
                                {"hostname": "ext-b", "device_id": "ec:fc:2f:5b:f2:b0"}], None, None)
        self.assertEqual(names.mesh_node_for("ec:fc:2f:5b:f2:b8"), "ext-b")
        self.assertEqual(names.mesh_node_for("ec:fc:2f:5b:f2:93"), "ext-a")
        self.assertIsNone(names.mesh_node_for("ec:fc:2f:5b:f2:80"))


class DiagnoseTest(unittest.TestCase):
    def test_healthy_network_has_no_warnings(self):
        result = diag.diagnose(base_state(), [], 25)
        self.assertNotIn("warning", result["summary"])
        self.assertNotIn("critical", result["summary"])

    def test_wan_outage_paired(self):
        events = [event(60, "Wan Ethernet connectivity has been disconnected", "WETH"),
                  event(59.5, "WAN Ethernet connectivity has been established", "WETH")]
        result = diag.diagnose(base_state(), events, 25)
        outage = next(f for f in result["findings"] if f["category"] == "wan")
        self.assertEqual(outage["evidence"]["outages"][0]["seconds"], 30)

    def test_flapping_client(self):
        events = []
        for i in range(10):
            events.append(event(600 - i * 60, "A WiFi device <44:07:0b:5f:8b:7c> has successfully "
                                              "connected to SSID (Device/WiFi/SSIDs/SSID[WLAN_2_4])"))
            events.append(event(599 - i * 60, "Device <44:07:0b:5f:8b:7c> was disconnected on SSID "
                                              "(Device/WiFi/SSIDs/SSID[WLAN_2_4])"))
        result = diag.diagnose(base_state(), events, 10)
        finding = next(f for f in result["findings"] if "repeatedly" in f["title"])
        device = finding["evidence"]["devices"][0]
        self.assertEqual(device["name"], "phone")
        self.assertEqual(device["median_session_minutes"], 1.0)
        self.assertEqual(device["current"]["signal_strength"], -60)

    def test_extender_auth_failures_attributed_to_mesh_node(self):
        events = [event(i, "A device failed to connect to SSID (ec:fc:2f:5b:f2:b8) because it "
                           "provided incorrect login information.", level="err") for i in range(6)]
        result = diag.diagnose(base_state(), events, 25)
        finding = next(f for f in result["findings"] if "Mesh node" in f["title"])
        self.assertEqual(finding["evidence"]["nodes"][0]["name"], "ext1")

    def test_recent_gateway_reboot_with_reason(self):
        events = [event(30, "WDG # Current boot was caused by Hard Reset (Power) = POR_RESET_STATUS: 0x80000000", "SYS")]
        result = diag.diagnose(base_state(device={"uptime": 1800}), events, 25)
        finding = next(f for f in result["findings"] if f["title"].startswith("Gateway rebooted"))
        self.assertIn("power", finding["likely_cause"])

    def test_backhaul_on_24ghz_only_is_weak(self):
        ext = base_state()["extenders"][0]
        ext["backhaul"]["wifiLinks"] = [
            {"band": "2.4", "signalStrength": -60, "linkQuality": "GOOD"},
            {"band": "5", "signalStrength": -80, "linkQuality": "BAD"},
        ]
        result = diag.diagnose(base_state(extenders=[ext]), [], 25)
        self.assertIn("Weak extender backhaul", titles(result))

    def test_backhaul_rssi0_field(self):
        ext = base_state()["extenders"][0]
        ext["backhaul"]["wifiLinks"] = [{"band": "6", "rssi0": -50, "linkQuality": "GOOD"}]
        result = diag.diagnose(base_state(extenders=[ext]), [], 25)
        self.assertNotIn("Weak extender backhaul", titles(result))

    def test_100mbps_port(self):
        port = {"name": "eth0", "role": "LAN", "status": "UP", "speed_mbps": 100, "duplex": "FULL",
                "rx": {}, "tx": {}}
        result = diag.diagnose(base_state(lan_ports=[port]), [], 25)
        self.assertIn("Ethernet link(s) negotiated below 1 Gbps", titles(result))

    def test_cgnat(self):
        result = diag.diagnose(base_state(wan_ipv4={"status": "Up", "address": "100.64.193.104"}), [], 25)
        self.assertIn("WAN IPv4 is behind carrier-grade NAT", titles(result))


class HistoryTest(unittest.TestCase):
    def test_detects_reboots_ip_change_and_throughput(self):
        t0 = NOW.timestamp() - 3600
        first = base_state(wan_stats={"rx": {"bytes": 0}, "tx": {"bytes": 0}})
        second = base_state(
            device={"uptime": 100, "firmware": "FW2"},
            wan_ipv4={"status": "Up", "address": "84.1.2.4"},
            wan_stats={"rx": {"bytes": 300 * 10**6}, "tx": {"bytes": 30 * 10**6}},
        )
        summary = diag.summarize_history([(t0, first), (t0 + 300, second)])
        changes = " | ".join(c["change"] for c in summary["changes"])
        self.assertIn("gateway rebooted", changes)
        self.assertIn("firmware FW1 -> FW2", changes)
        self.assertIn("WAN IP 84.1.2.3 -> 84.1.2.4", changes)
        self.assertEqual(sum(h["peak_rx_mbps"] for h in summary["hourly"]), 8.0)

    def test_empty(self):
        self.assertEqual(diag.summarize_history([])["snapshots"], 0)


if __name__ == "__main__":
    unittest.main()
