"""Turn raw Sagemcom 5598 state, event log and snapshot history into
findings: what looks wrong, the evidence, and the likely root cause.

Pure functions over plain dicts (as returned by `Sagemcom5598` and
`sagemcom5598_history`), so they can be tested without a router."""

import ipaddress
import re
import statistics
from collections import Counter, defaultdict
from datetime import datetime, timedelta, timezone

WEAK_CLIENT_SIGNAL_DBM = -75
WEAK_BACKHAUL_SIGNAL_DBM = -70
FLAP_MIN_DISCONNECTS = 6          # in the window, and at least...
FLAP_MIN_PER_HOUR = 0.5           # ...this rate, before a client counts as flapping
AUTH_FAILURE_MIN = 5
TX_ERROR_RATIO = 0.02
REBOOT_CLUSTER = timedelta(minutes=15)
SEVERITY_ORDER = {"critical": 0, "warning": 1, "info": 2}

_MAC = r"(?P<mac>[0-9a-fA-F]{2}(?::[0-9a-fA-F]{2}){5})"
EVENT_PATTERNS = [
    ("wifi_connect", re.compile(rf"A WiFi device <{_MAC}> has successfully connected to SSID \(.*SSID\[(?P<ssid>[^\]]+)\]\)")),
    ("wifi_disconnect", re.compile(rf"Device <{_MAC}> was disconnected on SSID \(.*SSID\[(?P<ssid>[^\]]+)\]\)")),
    ("wifi_auth_failure", re.compile(rf"A device failed to connect to SSID \({_MAC}\) because it provided incorrect login")),
    ("boot_reason", re.compile(r"Current boot was caused by (?P<reason>[^=]+?)\s*=")),
    ("power_up", re.compile(r"The Modem has successfully powered up")),
    ("wan_down", re.compile(r"WAN Ethernet connectivity has been disconnected", re.I)),
    ("wan_up", re.compile(r"WAN Ethernet connectivity has been established", re.I)),
    ("wan_dhcp_stopped", re.compile(r"WAN DHCP client \((?P<id>\d+)\) stopped")),
    ("wan_dhcp_started", re.compile(r"WAN DHCP client \((?P<id>\d+)\) started")),
    ("tr069_failed", re.compile(r"TR-069 connectivity to \((?P<host>[^)]+)\) has failed")),
    ("tr069_session", re.compile(r"TR-069 connectivity to \((?P<host>[^)]+)\) has been initiated")),
    ("gui_login", re.compile(r"GUI login was successful for user (?P<user>\S+) from (?P<ip>\S+)")),
    ("gui_logout", re.compile(r"GUI logout was successful for user (?P<user>\S+) from (?P<ip>\S+)")),
    ("tr069_closed", re.compile(r"TR-069 connectivity to \((?P<host>[^)]+)\) has been closed")),
    ("dns_active", re.compile(r"DNS name resolution is now active")),
    ("dhcp_server_active", re.compile(r"The LAN DHCP Server is active")),
    ("gui_login_failed", re.compile(r"GUI login .*(fail|incorrect|denied)", re.I)),
    ("lan_port_up", re.compile(r"An Ethernet port is now connected \((?P<port>\d+)/(?P<speed>\d+)/(?P<duplex>\w+)\)")),
    ("lan_port_down", re.compile(r"An Ethernet port is now disconnected", re.I)),
]

BOOT_REASONS = {
    "power": "power loss or power cycle (outage, plug pulled, failing power adapter)",
    "watchdog": "watchdog reset - the firmware hung or crashed",
    "software": "software-initiated reboot (via the GUI, the ISP's TR-069 management, or a firmware update)",
}


def parse_time(value: str) -> datetime:
    return datetime.fromisoformat(value)


def classify(event: dict) -> tuple[str, dict]:
    for kind, pattern in EVENT_PATTERNS:
        match = pattern.search(event["message"])
        if match:
            groups = match.groupdict()
            if "mac" in groups:
                groups["mac"] = groups["mac"].lower()
            return kind, groups
    return "other", {}


def ssid_band(ssid: str) -> str | None:
    """`WLAN_2_4` -> "2.4", `WL_BACKHAUL_6` -> "6", ..."""
    if ssid.endswith("2_4"):
        return "2.4"
    tail = ssid.rsplit("_", 1)[-1]
    return tail if tail in ("5", "6") else None


def is_private_mac(mac: str) -> bool:
    """Locally administered MAC - phones/laptops using per-network random
    MACs. Such a device can show up under several MACs over time."""
    return int(mac[:2], 16) & 0x02 == 0x02


class Names:
    """Resolves MACs seen in the event log to something a human knows."""

    def __init__(self, hosts: list[dict], extenders: list[dict], gateway_id: str | None,
                 gateway_name: str | None, devices: list[dict] = ()) -> None:
        self.by_mac = {h["mac"].lower(): h["name"] for h in hosts if h.get("mac")}
        self.by_mac.update({d["mac"].lower(): d["name"] for d in devices if d.get("mac")})
        self.mesh_nodes = {e["device_id"].lower(): e["hostname"] for e in extenders if e.get("device_id")}
        if gateway_id:
            self.mesh_nodes[gateway_id.lower()] = gateway_name or "gateway"

    def mesh_node_for(self, mac: str) -> str | None:
        """Mesh nodes use a block of consecutive MACs starting at their
        deviceId for their radios (backhaul station, per-band BSSIDs)."""
        mac = mac.lower()
        if mac in self.mesh_nodes:
            return self.mesh_nodes[mac]
        best = None
        for node_id, name in self.mesh_nodes.items():
            if node_id[:14] != mac[:14]:
                continue
            diff = int(mac[15:], 16) - int(node_id[15:], 16)
            if 0 <= diff < 0x20 and (best is None or diff < best[0]):
                best = (diff, name)
        return best[1] if best else None

    def resolve(self, mac: str) -> dict:
        mac = mac.lower()
        node = self.mesh_node_for(mac)
        return {
            "mac": mac,
            "name": node or self.by_mac.get(mac),
            "kind": "mesh_node" if node else ("client" if mac in self.by_mac else "unknown"),
            "private_mac": is_private_mac(mac),
        }


def names_from_state(state: dict) -> Names:
    gateway = state.get("gateway") or {}
    return Names(state.get("hosts", []), state.get("extenders", []), gateway.get("device_id"),
                 gateway.get("hostname"), state.get("devices", []))


def events_in_window(events: list[dict], hours: float, now: datetime | None = None) -> list[dict]:
    now = now or datetime.now(timezone.utc)
    start = now - timedelta(hours=hours)
    return [e for e in events if parse_time(e["time"]) >= start]


def _finding(severity: str, title: str, evidence, cause: str, suggestion: str, category: str) -> dict:
    return {
        "severity": severity,
        "category": category,
        "title": title,
        "evidence": evidence,
        "likely_cause": cause,
        "suggestion": suggestion,
    }


def summarize_events(events: list[dict], names: Names, hours: float, own_ip: str | None = None) -> dict:
    """Aggregate the event log over the window into per-kind and
    per-device counts, so a few thousand lines become a readable picture."""
    window = events_in_window(events, hours)
    kinds = Counter()
    per_device = defaultdict(lambda: Counter())
    ssids = defaultdict(set)
    gui_logins = Counter()
    other = Counter()
    for event in window:
        kind, groups = classify(event)
        kinds[kind] += 1
        if "mac" in groups:
            per_device[groups["mac"]][kind] += 1
            if groups.get("ssid"):
                ssids[groups["mac"]].add(groups["ssid"])
        if kind == "gui_login":
            source = groups["ip"] + (" (this MCP server)" if groups["ip"] == own_ip else "")
            gui_logins[source] += 1
        if kind == "other":
            other[f"{event['module']}: {event['message']}"] += 1

    devices = []
    for mac, counts in per_device.items():
        devices.append({
            **names.resolve(mac),
            "connects": counts["wifi_connect"],
            "disconnects": counts["wifi_disconnect"],
            "auth_failures": counts["wifi_auth_failure"],
            "ssids": sorted(ssids[mac]),
        })
    devices.sort(key=lambda d: -(d["connects"] + d["disconnects"] + d["auth_failures"]))

    return {
        "window_hours": hours,
        "total_events": len(window),
        "first_event": window[0]["time"] if window else None,
        "last_event": window[-1]["time"] if window else None,
        "event_counts": dict(kinds.most_common()),
        "wifi_devices": devices,
        "gui_logins_by_source": dict(gui_logins),
        "other_events": [{"event": text, "count": n} for text, n in other.most_common(30)],
    }


def _boot_reason_text(reason: str) -> str:
    lowered = reason.lower()
    for key, text in BOOT_REASONS.items():
        if key in lowered:
            return text
    return reason


def _check_reboots(state: dict, window: list[tuple[dict, str, dict]], hours: float, now: datetime) -> list[dict]:
    findings = []
    tr069 = [parse_time(e["time"]) for e, kind, _ in window if kind == "tr069_session"]

    uptime = state["device"]["uptime"]
    if uptime < hours * 3600:
        boot = now - timedelta(seconds=uptime)
        reasons = [g["reason"] for _, kind, g in window if kind == "boot_reason"]
        cause = _boot_reason_text(reasons[-1]) if reasons else "unknown - no boot reason in the event log"
        findings.append(_finding(
            "warning", f"Gateway rebooted {boot.astimezone().strftime('%Y-%m-%d %H:%M')}",
            {"device_uptime_seconds": uptime, "boot_reason_logged": reasons[-1] if reasons else None},
            cause,
            "If it repeats: check the power adapter/socket for power resets; for watchdog "
            "resets note the firmware version and escalate to the ISP.",
            "reboot",
        ))

    recent = []
    for ext in state.get("extenders", []):
        ext_uptime = int(ext.get("uptime") or 0)
        if ext_uptime and ext_uptime < hours * 3600:
            recent.append((now - timedelta(seconds=ext_uptime), ext))
    if recent:
        recent.sort(key=lambda pair: pair[0])
        clustered = len(recent) > 1 and recent[-1][0] - recent[0][0] <= REBOOT_CLUSTER
        gateway_too = uptime < hours * 3600 and abs((now - timedelta(seconds=uptime)) - recent[0][0]) <= REBOOT_CLUSTER
        near_tr069 = [t.astimezone().strftime("%H:%M") for t in tr069
                      if any(abs(t - boot) <= timedelta(minutes=30) for boot, _ in recent)]
        if gateway_too:
            cause = "The extenders restarted together with the gateway (they lose their mesh controller when it reboots)."
        elif clustered:
            cause = ("All these extenders restarted together while the gateway stayed up: typically an "
                     "ISP-pushed extender firmware update (check whether firmware changed), a power outage "
                     "on the circuit they share, or a mesh reconfiguration (SSID/password/MLO change).")
        else:
            cause = "Individual extender restart: power, overheating, or a crash of the extender firmware."
        findings.append(_finding(
            "warning", f"{len(recent)} extender(s) rebooted in the last {hours:g}h",
            {
                "extenders": [
                    {"hostname": ext["hostname"], "rebooted_at": boot.astimezone().isoformat(timespec="minutes"),
                     "firmware": ext.get("firmware")}
                    for boot, ext in recent
                ],
                "tr069_sessions_within_30min": near_tr069,
            },
            cause,
            "Compare extender firmware in history_summary; if reboots recur, check each extender's power supply.",
            "reboot",
        ))
    return findings


def _check_wan(state: dict, window: list[tuple[dict, str, dict]], hours: float) -> list[dict]:
    findings = []
    wan = state.get("wan_ipv4", {})
    if wan.get("status") != "Up":
        findings.append(_finding(
            "critical", f"WAN is {wan.get('status')}",
            {"wan_ipv4": wan, "wan_status": state.get("wan_status")},
            "No working internet uplink: fiber/ONT problem, WAN cable unplugged, or no DHCP lease from the ISP.",
            "Check the fiber/ONT lights and the cable into the WAN port; check for an ISP outage.",
            "wan",
        ))

    outages, down_at = [], None
    for event, kind, _ in window:
        if kind == "wan_down":
            down_at = parse_time(event["time"])
        elif kind == "wan_up" and down_at:
            outages.append({"down": down_at.isoformat(), "up": event["time"],
                            "seconds": int((parse_time(event["time"]) - down_at).total_seconds())})
            down_at = None
    if down_at:
        outages.append({"down": down_at.isoformat(), "up": None, "seconds": None})
    if outages:
        findings.append(_finding(
            "critical" if len(outages) > 2 else "warning",
            f"WAN link dropped {len(outages)} time(s) in the last {hours:g}h",
            {"outages": outages},
            "Short drops (seconds) point at the physical WAN link: fiber/ONT resync, a loose or damaged WAN "
            "cable, or ISP-side maintenance. Drops at the same time daily suggest scheduled ISP maintenance.",
            "Reseat/replace the WAN cable; if drops continue, the ISP should check the fiber line/ONT.",
            "wan",
        ))

    dhcp_restarts = sum(1 for _, kind, _ in window if kind == "wan_dhcp_stopped")
    if dhcp_restarts and not outages:
        findings.append(_finding(
            "warning", "WAN DHCP client restarted without a link drop",
            {"dhcp_client_stops": dhcp_restarts},
            "Lease renewal problems at the ISP side: the link stayed up but the IP lease was lost.",
            "Escalate to the ISP with the timestamps from event_log(module='DHCPC').",
            "wan",
        ))

    address = wan.get("address")
    try:
        if address and ipaddress.ip_address(address) in ipaddress.ip_network("100.64.0.0/10"):
            findings.append(_finding(
                "info", "WAN IPv4 is behind carrier-grade NAT",
                {"wan_address": address},
                "The ISP shares public IPv4 addresses (CGNAT, 100.64.0.0/10). Inbound IPv4 port forwarding "
                "cannot work; outbound traffic is fine.",
                "Use IPv6 for inbound services, or ask the ISP for a public IPv4 address.",
                "wan",
            ))
    except ValueError:
        pass

    failed = [e["time"] for e, kind, _ in window if kind == "tr069_failed"]
    if failed:
        findings.append(_finding(
            "info", "ISP remote management (TR-069) connection failed",
            {"failed_at": failed},
            "The gateway could not reach the ISP's ACS - harmless once, but repeated failures mean the ISP "
            "cannot manage or update the device.",
            "Only relevant if it persists; the ISP can see this from their ACS side.",
            "wan",
        ))
    return findings


def _check_wifi_events(window: list[tuple[dict, str, dict]], names: Names, hours: float,
                       devices_now: dict[str, dict]) -> list[dict]:
    findings = []
    auth = Counter(g["mac"] for _, kind, g in window if kind == "wifi_auth_failure")
    mesh_auth, client_auth = [], []
    for mac, count in auth.most_common():
        if count < AUTH_FAILURE_MIN:
            continue
        who = names.resolve(mac)
        (mesh_auth if who["kind"] == "mesh_node" else client_auth).append({**who, "failures": count})
    if mesh_auth:
        findings.append(_finding(
            "warning", "Mesh node radios repeatedly rejected with wrong credentials",
            {"nodes": mesh_auth},
            "A mesh extender keeps trying to associate with credentials the network no longer accepts - "
            "typically stale mesh/backhaul credentials after an SSID/password change or an extender that "
            "was re-paired to a different node. It wastes airtime and can destabilise the backhaul.",
            "Re-pair the named extender (WPS on gateway + extender), or factory-reset it and add it again.",
            "wifi",
        ))
    if client_auth:
        findings.append(_finding(
            "warning", "Devices repeatedly failing wifi authentication",
            {"devices": client_auth},
            "The device has an outdated wifi password saved (typically after a password change), "
            "or an unknown device is trying to guess it.",
            "Update the saved wifi password on the device, or forget the network and reconnect.",
            "wifi",
        ))

    sessions = defaultdict(list)
    for event, kind, groups in window:
        if kind in ("wifi_connect", "wifi_disconnect"):
            sessions[groups["mac"]].append((parse_time(event["time"]), kind, groups["ssid"]))
    flappers, backhaul_flaps = [], []
    for mac, items in sessions.items():
        disconnects = sum(1 for _, kind, _ in items if kind == "wifi_disconnect")
        if disconnects < FLAP_MIN_DISCONNECTS or disconnects / hours < FLAP_MIN_PER_HOUR:
            continue
        durations = [
            (items[i + 1][0] - t).total_seconds()
            for i, (t, kind, _) in enumerate(items[:-1])
            if kind == "wifi_connect" and items[i + 1][1] == "wifi_disconnect"
        ]
        ssids = sorted({ssid for _, _, ssid in items})
        entry = {
            **names.resolve(mac),
            "disconnects": disconnects,
            "per_hour": round(disconnects / hours, 1),
            "median_session_minutes": round(statistics.median(durations) / 60, 1) if durations else None,
            "ssids": ssids,
            "bands": sorted({b for b in map(ssid_band, ssids) if b}),
        }
        now = devices_now.get(mac)
        if now:
            entry["current"] = {k: now.get(k) for k in ("band", "signal_strength", "link_quality", "connected_via")}
        (backhaul_flaps if any("BACKHAUL" in s for s in ssids) else flappers).append(entry)

    if backhaul_flaps:
        findings.append(_finding(
            "warning", "Mesh backhaul links keep reconnecting",
            {"nodes": sorted(backhaul_flaps, key=lambda e: -e["disconnects"])},
            "The wireless link between an extender and its parent drops repeatedly: the extender is too far "
            "from its parent, or there's interference on the backhaul channel. Every drop interrupts all "
            "clients on that extender.",
            "Move the extender closer to its parent (aim for better than -65 dBm), or wire it with Ethernet backhaul.",
            "mesh",
        ))
    if flappers:
        flappers.sort(key=lambda e: -e["disconnects"])
        findings.append(_finding(
            "warning", f"{len(flappers)} wifi device(s) connecting and disconnecting repeatedly",
            {"devices": flappers},
            "Typical causes, in order: weak signal at the device's location; aggressive power saving on "
            "IoT/battery devices (short, regular sessions); band steering pushing a device that only "
            "handles one band well (all flaps on 2.4 GHz); a phone with private MAC re-joining.",
            "Check `current.signal_strength` for each; for a 2.4 GHz-only IoT device consider a separate "
            "2.4 GHz SSID or disabling band steering; move the device or add/relocate an extender.",
            "wifi",
        ))
    return findings


def _check_signal(state: dict) -> list[dict]:
    findings = []
    weak = [
        {k: d.get(k) for k in ("name", "mac", "band", "signal_strength", "link_quality", "connected_via")}
        for d in state.get("devices", [])
        if d.get("connection") == "wireless" and d.get("signal_strength") is not None
        and d["signal_strength"] < WEAK_CLIENT_SIGNAL_DBM
    ]
    if weak:
        findings.append(_finding(
            "warning", f"{len(weak)} wifi client(s) with weak signal (< {WEAK_CLIENT_SIGNAL_DBM} dBm)",
            {"devices": weak},
            "The device is far from the node it's connected to (or behind walls/floors). Expect low speeds and drops.",
            "Move the device or the nearest mesh node; check whether it sticks to a far node instead of roaming.",
            "wifi",
        ))

    extenders = {e["hostname"]: e for e in state.get("extenders", [])}
    weak_bh, multi_hop = [], []
    for ext in extenders.values():
        backhaul = ext.get("backhaul") or {}
        if backhaul.get("linkType") == "Ethernet":
            if backhaul.get("speed") and backhaul["speed"] < 1000:
                weak_bh.append({"hostname": ext["hostname"], "ethernet_speed_mbps": backhaul["speed"],
                                "problem": "Ethernet backhaul below 1 Gbps - cable or switch port"})
            continue
        # Some firmware reports `rssi0` instead of `signalStrength`; 0 means "band not in use".
        links = [
            {"band": l.get("band"), "signal_dbm": l.get("signalStrength", l.get("rssi0")), "quality": l.get("linkQuality")}
            for l in backhaul.get("wifiLinks", [])
        ]
        links = [l for l in links if l["signal_dbm"]]
        # 5/6 GHz carry the real backhaul throughput; a 2.4 GHz-only link is slow even when "good".
        best_high = max((l["signal_dbm"] for l in links if l["band"] in ("5", "6") and l["quality"] != "BAD"), default=None)
        if best_high is None or best_high < WEAK_BACKHAUL_SIGNAL_DBM:
            weak_bh.append({"hostname": ext["hostname"], "parent": ext.get("parent"), "links": links,
                            "problem": "no usable 5/6 GHz backhaul" if best_high is None else "weak 5/6 GHz backhaul"})
        parent = extenders.get(ext.get("parent"))
        if parent and (parent.get("backhaul") or {}).get("linkType") != "Ethernet":
            multi_hop.append({"hostname": ext["hostname"], "via": ext["parent"]})
    if weak_bh:
        findings.append(_finding(
            "warning", "Weak extender backhaul",
            {"extenders": weak_bh},
            "The extender's link to its parent is poor, so every client on it is capped by that link - "
            "clients show good signal to the extender but still get slow internet.",
            "Move the extender closer to its parent (-65 dBm or better on 5/6 GHz) or use Ethernet backhaul.",
            "mesh",
        ))
    if multi_hop:
        findings.append(_finding(
            "info", "Extenders chained over wireless (multi-hop)",
            {"extenders": multi_hop},
            "Each wireless hop roughly halves available throughput and adds latency.",
            "Where possible, connect the first-hop extender via Ethernet or place extenders so each reaches the gateway directly.",
            "mesh",
        ))
    firmwares = {e.get("firmware") for e in extenders.values()}
    if len(firmwares) > 1:
        findings.append(_finding(
            "info", "Extenders run different firmware versions",
            {"firmware_by_extender": {e["hostname"]: e.get("firmware") for e in extenders.values()}},
            "A pending or partially applied firmware rollout; mixed versions can cause mesh quirks.",
            "Usually resolves itself after the ISP rollout completes; otherwise reboot the outdated extender.",
            "mesh",
        ))
    return findings


def _check_ethernet(state: dict) -> list[dict]:
    findings = []
    slow = [
        {k: p.get(k) for k in ("name", "alias", "role", "speed_mbps", "duplex")}
        for p in state.get("lan_ports", [])
        if p.get("status") == "UP" and p.get("speed_mbps") and p["speed_mbps"] < 1000
    ]
    slow_devices = [
        {k: d.get(k) for k in ("name", "mac", "connected_via", "link_speed_mbps")}
        for d in state.get("devices", [])
        if d.get("connection") == "wired" and d.get("link_speed_mbps") and d["link_speed_mbps"] < 1000
    ]
    if slow or slow_devices:
        findings.append(_finding(
            "warning", "Ethernet link(s) negotiated below 1 Gbps",
            {"gateway_ports": slow, "wired_devices": slow_devices},
            "A 100 Mbps link on gigabit hardware almost always means a damaged cable, a cable with only "
            "2 working pairs (old Cat5 / flat cable), a bad connector - or a device that is 100 Mbps-only.",
            "Swap the cable (Cat5e/Cat6); if it stays at 100 Mbps, check the device's own port speed.",
            "ethernet",
        ))
    duplex = [p["name"] for p in state.get("lan_ports", []) if p.get("status") == "UP" and p.get("duplex") == "HALF"]
    if duplex:
        findings.append(_finding(
            "warning", "Ethernet port running half duplex", {"ports": duplex},
            "Duplex mismatch or a bad cable - causes collisions and very poor throughput.",
            "Replace the cable; make sure both ends use auto-negotiation.",
            "ethernet",
        ))
    errors = [
        {"name": p["name"], "role": p.get("role"),
         "rx_errors": p["rx"].get("packetserrors"), "tx_errors": p["tx"].get("packetserrors"),
         "rx_discards": p["rx"].get("packetsdiscards"), "tx_discards": p["tx"].get("packetsdiscards")}
        for p in state.get("lan_ports", [])
        if p["rx"].get("packetserrors") or p["tx"].get("packetserrors")
    ]
    if errors:
        findings.append(_finding(
            "info", "Packet errors counted on Ethernet port(s) since boot",
            {"ports": errors},
            "Errors on a wired port point at cable/connector problems or electrical interference.",
            "Use history_summary to see whether they are still increasing; replace the cable if so.",
            "ethernet",
        ))
    return findings


def _check_wifi_stats(state: dict) -> list[dict]:
    noisy = []
    for band, data in (state.get("wifi_stats") or {}).items():
        tx = data.get("tx") or {}
        packets, errs = int(tx.get("packets") or 0), int(tx.get("packetserrors") or 0)
        if packets and errs / packets > TX_ERROR_RATIO:
            noisy.append({"band": band, "tx_packets": packets, "tx_errors": errs, "error_ratio": round(errs / packets, 3)})
    if not noisy:
        return []
    return [_finding(
        "info", "High wifi transmit error ratio on the gateway",
        {"bands": noisy},
        "Retransmissions from interference/congestion on the channel or clients at the edge of range.",
        "Check the channel in wifi_details; on 2.4 GHz prefer 1, 6 or 11; relocate far clients.",
        "wifi",
    )]


def _check_ntp(state: dict) -> list[dict]:
    ntp = state.get("ntp") or {}
    if not ntp or ntp.get("status") == "SYNCHRONIZED":
        return []
    return [_finding(
        "warning", f"Router clock not synchronized ({ntp.get('status')})",
        {"ntp": ntp},
        "No NTP sync - usually because the WAN is down or NTP servers are unreachable. Log timestamps are unreliable.",
        "Fix WAN connectivity first; the clock syncs automatically afterwards.",
        "system",
    )]


def summarize_history(snapshots: list[tuple[float, dict]]) -> dict:
    """Condense (timestamp, snapshot) pairs into changes and hourly trends."""
    if not snapshots:
        return {"snapshots": 0, "note": "No history collected yet - run sagemcom5598_history.py or keep the MCP server running with collection enabled."}

    def ts_iso(ts: float) -> str:
        return datetime.fromtimestamp(ts).astimezone().isoformat(timespec="minutes")

    changes = []
    gaps = []
    intervals = [b - a for (a, _), (b, _) in zip(snapshots, snapshots[1:])]
    typical = statistics.median(intervals) if intervals else None
    hourly = defaultdict(lambda: {"rx_bytes": 0, "tx_bytes": 0, "seconds": 0, "peak_rx_mbps": 0.0, "peak_tx_mbps": 0.0, "clients": []})
    client_seen = defaultdict(list)
    backhaul = defaultdict(lambda: defaultdict(list))

    for i, (ts, snap) in enumerate(snapshots):
        bucket = hourly[datetime.fromtimestamp(ts).astimezone().strftime("%Y-%m-%d %H:00")]
        bucket["clients"].append(len(snap.get("devices", [])))
        for dev in snap.get("devices", []):
            client_seen[dev["mac"]].append((ts, dev))
        for ext in snap.get("extenders", []):
            for link in (ext.get("backhaul") or {}).get("wifiLinks", []):
                signal = link.get("signalStrength", link.get("rssi0"))
                if signal:
                    backhaul[ext["hostname"]][link["band"]].append(signal)
        if i == 0:
            continue
        prev_ts, prev = snapshots[i - 1]
        dt = ts - prev_ts
        if typical and dt > 3 * typical:
            gaps.append({"from": ts_iso(prev_ts), "to": ts_iso(ts), "minutes": round(dt / 60)})
        if snap["device"]["uptime"] < prev["device"]["uptime"]:
            changes.append({"time": ts_iso(ts - snap["device"]["uptime"]), "change": "gateway rebooted"})
        if snap["device"].get("firmware") != prev["device"].get("firmware"):
            changes.append({"time": ts_iso(ts), "change": f"gateway firmware {prev['device'].get('firmware')} -> {snap['device'].get('firmware')}"})
        if snap["wan_ipv4"].get("address") != prev["wan_ipv4"].get("address"):
            changes.append({"time": ts_iso(ts), "change": f"WAN IP {prev['wan_ipv4'].get('address')} -> {snap['wan_ipv4'].get('address')}"})
        if snap["wan_ipv4"].get("status") != prev["wan_ipv4"].get("status"):
            changes.append({"time": ts_iso(ts), "change": f"WAN status {prev['wan_ipv4'].get('status')} -> {snap['wan_ipv4'].get('status')}"})
        prev_ext = {e.get("device_id") or e["hostname"]: e for e in prev.get("extenders", [])}
        for ext in snap.get("extenders", []):
            old = prev_ext.pop(ext.get("device_id") or ext["hostname"], None)
            if old is None:
                changes.append({"time": ts_iso(ts), "change": f"extender {ext['hostname']} joined the mesh"})
                continue
            if int(ext.get("uptime") or 0) < int(old.get("uptime") or 0):
                changes.append({"time": ts_iso(ts - int(ext['uptime'])), "change": f"extender {ext['hostname']} rebooted"})
            if ext.get("firmware") != old.get("firmware"):
                changes.append({"time": ts_iso(ts), "change": f"extender {ext['hostname']} firmware {old.get('firmware')} -> {ext.get('firmware')}"})
            if ext.get("parent") != old.get("parent"):
                changes.append({"time": ts_iso(ts), "change": f"extender {ext['hostname']} parent {old.get('parent')} -> {ext.get('parent')}"})
            old_type = (old.get("backhaul") or {}).get("linkType")
            new_type = (ext.get("backhaul") or {}).get("linkType")
            if old_type != new_type:
                changes.append({"time": ts_iso(ts), "change": f"extender {ext['hostname']} backhaul {old_type} -> {new_type}"})
        for gone in prev_ext.values():
            changes.append({"time": ts_iso(ts), "change": f"extender {gone['hostname']} left the mesh"})
        prev_ports = {p["name"]: p for p in prev.get("lan_ports", [])}
        for port in snap.get("lan_ports", []):
            old = prev_ports.get(port["name"])
            if old and (old.get("status"), old.get("speed_mbps")) != (port.get("status"), port.get("speed_mbps")):
                changes.append({"time": ts_iso(ts), "change": f"port {port['name']} {old.get('status')}/{old.get('speed_mbps')} -> {port.get('status')}/{port.get('speed_mbps')} Mbps"})
            if old:
                for direction in ("rx", "tx"):
                    delta = port[direction].get("packetserrors", 0) - old[direction].get("packetserrors", 0)
                    if delta > 0:
                        changes.append({"time": ts_iso(ts), "change": f"port {port['name']} +{delta} {direction} errors"})

        rx = snap["wan_stats"]["rx"]["bytes"] - prev["wan_stats"]["rx"]["bytes"]
        tx = snap["wan_stats"]["tx"]["bytes"] - prev["wan_stats"]["tx"]["bytes"]
        if rx >= 0 and tx >= 0 and dt > 0:  # counters reset on reboot
            bucket["rx_bytes"] += rx
            bucket["tx_bytes"] += tx
            bucket["seconds"] += dt
            bucket["peak_rx_mbps"] = max(bucket["peak_rx_mbps"], rx * 8 / dt / 1e6)
            bucket["peak_tx_mbps"] = max(bucket["peak_tx_mbps"], tx * 8 / dt / 1e6)

    hours_out = []
    for hour, b in sorted(hourly.items()):
        hours_out.append({
            "hour": hour,
            "wan_rx_GB": round(b["rx_bytes"] / 1e9, 2),
            "wan_tx_GB": round(b["tx_bytes"] / 1e9, 2),
            "avg_rx_mbps": round(b["rx_bytes"] * 8 / b["seconds"] / 1e6, 1) if b["seconds"] else None,
            "peak_rx_mbps": round(b["peak_rx_mbps"], 1),
            "peak_tx_mbps": round(b["peak_tx_mbps"], 1),
            "clients_min": min(b["clients"]),
            "clients_max": max(b["clients"]),
        })

    total = len(snapshots)
    clients = []
    for mac, seen in client_seen.items():
        signals = [d["signal_strength"] for _, d in seen if d.get("signal_strength") is not None]
        vias = [d.get("connected_via") for _, d in seen]
        bands = [d.get("band") for _, d in seen]
        clients.append({
            "name": seen[-1][1].get("name"),
            "mac": mac,
            "connection": seen[-1][1].get("connection"),
            "present_pct": round(100 * len(seen) / total),
            "first_seen": ts_iso(seen[0][0]),
            "last_seen": ts_iso(seen[-1][0]),
            "signal_dbm": {"min": min(signals), "avg": round(statistics.mean(signals)), "max": max(signals)} if signals else None,
            "node_changes": sum(1 for a, b in zip(vias, vias[1:]) if a != b),
            "band_changes": sum(1 for a, b in zip(bands, bands[1:]) if a != b),
            "nodes": sorted({v for v in vias if v}),
        })
    clients.sort(key=lambda c: (c["present_pct"], c["name"] or ""))

    return {
        "snapshots": total,
        "from": ts_iso(snapshots[0][0]),
        "to": ts_iso(snapshots[-1][0]),
        "typical_interval_seconds": round(typical) if typical else None,
        "collection_gaps": gaps,
        "changes": sorted(changes, key=lambda c: c["time"]),
        "hourly": hours_out,
        "extender_backhaul_dbm": {
            host: {band: {"min": min(v), "avg": round(statistics.mean(v)), "max": max(v)} for band, v in bands.items()}
            for host, bands in backhaul.items()
        },
        "clients": clients,
    }


def diagnose(state: dict, events: list[dict], hours: float = 25,
             history: list[tuple[float, dict]] | None = None, own_ip: str | None = None) -> dict:
    """All checks over current state + event log window + optional history."""
    now = datetime.now(timezone.utc)
    names = names_from_state(state)
    window = [(e, *classify(e)) for e in events_in_window(events, hours, now)]
    devices_now = {d["mac"].lower(): d for d in state.get("devices", []) if d.get("mac")}

    findings = []
    findings += _check_wan(state, window, hours)
    findings += _check_reboots(state, window, hours, now)
    findings += _check_wifi_events(window, names, hours, devices_now)
    findings += _check_signal(state)
    findings += _check_ethernet(state)
    findings += _check_wifi_stats(state)
    findings += _check_ntp(state)

    history_summary = summarize_history(history or [])
    if history_summary.get("changes"):
        findings.append(_finding(
            "info", "Changes seen in collected history",
            {"changes": history_summary["changes"][-40:]},
            "Chronology of reboots, firmware, WAN IP, mesh and port changes detected between snapshots.",
            "Correlate with the user's complaint times.",
            "history",
        ))
    if history_summary.get("collection_gaps"):
        findings.append(_finding(
            "info", "Gaps in history collection",
            {"gaps": history_summary["collection_gaps"]},
            "The collector could not reach/log in to the router in these periods - the router was down, "
            "unreachable from the collector, or another admin session was active.",
            "Compare with WAN outages / reboots above.",
            "history",
        ))

    findings.sort(key=lambda f: SEVERITY_ORDER[f["severity"]])
    return {
        "generated_at": now.astimezone().isoformat(timespec="seconds"),
        "window_hours": hours,
        "summary": dict(Counter(f["severity"] for f in findings)),
        "findings": findings,
        "data_coverage": {
            "event_log_oldest": events[0]["time"] if events else None,
            "events_in_window": len(window),
            "history_snapshots": history_summary.get("snapshots", 0),
            "history_from": history_summary.get("from"),
        },
    }
