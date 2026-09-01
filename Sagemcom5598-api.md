# Sagemcom F@st 5598 — API reference

All endpoints observed on a Sagemcom F@st 5598 (model `F5598T`, Delta Fiber
firmware `SGQA530011400P`), reverse-engineered from HAR captures of the
router's own Angular web UI (`http://192.168.1.254/`) and from live testing.
The API is undocumented and unofficial.

Every endpoint returns/accepts JSON, wrapped in a top-level array — i.e.
`GET` responses look like `[{ ... }]`, so callers index `[0]`. Auth state is
tracked server-side (no session cookie); only one LAN admin session is
allowed at a time.

Legend: **Used** = implemented in `sagemcom5598.py`. **Seen** = observed in
HAR captures but not (yet) wrapped by the module. **Status** = HTTP status
observed in captures.

## Authentication & session

| Method | Path | Status | Used by | Description |
|---|---|---|---|---|
| GET | `/api/v1/open` | 200 | `login()` | Unauthenticated info endpoint, called before login. Returns WAN status, current router time, serial number, external/internal firmware versions, uptime, gateway IP. |
| POST | `/api/v2/login-params` | 200 | `login()` | First step of login. Body: `login`. Returns a per-attempt `salt` and `nonce` used to compute the challenge-response hash (SHA-512-crypt of the password, mixed with nonce/cnonce — see `sagemcom5598.py` for the reimplementation). |
| POST | `/api/v1/login` | 204 | `login()` | Second step of login. Body: `login`, `auth_key` (computed from the salt/nonce), `cnonce`. Returns 204 with no body on success; a 400 on wrong password. |
| POST | `/api/v1/logout` | 204 | `logout()` | Ends the authenticated session. |
| GET | `/api/v1/authenticated` | 200 | — | Seen. Returns `{"authenticated": true/false}` — used by the UI to check session state on page load/refresh. |
| GET | `/api/v1/session-count` | 200 | — | Seen. Returns `{"sessions": 1}` — number of active admin sessions (the router allows only one). |
| GET | `/api/v1/session-timeout` | 200 | — | Seen. Returns `{"timeout": "10"}` — idle session timeout in minutes. |
| GET | `/api/v1/user` | 200 | — | Seen. Returns the admin user list, e.g. `{"user": "beheer", "role": "support-user", "requirePasswordChange": true}`. |

## Device / system info

| Method | Path | Status | Used by | Description |
|---|---|---|---|---|
| GET | `/api/v1/device/features` | 200 | — | Seen. Feature/module flags used to drive the UI (e.g. `wifi` enabled, `guest` submodule, `bandsteering` submodule, etc.) — a capability map rather than live state. |
| GET | `/api/v1/ui/language` | **400** | — | Seen, not working. Consistently returned a 400 exception (`domain: /api/v1/ui/language`) in captures — endpoint exists but appears unimplemented/unsupported on this firmware. |

## WAN

| Method | Path | Status | Used by | Description |
|---|---|---|---|---|
| GET | `/api/v1/wan/status` | 200 | — | Seen. Minimal WAN link status: `{"status": "Up", "lastchange": <seconds>}`. |
| GET | `/api/v1/wan/ipv4` | 200 | — | Seen. Full WAN IPv4 config/state: addressing type (DHCP/static), WAN mode, address, subnet, gateway, uptime, MAC address, PPP username/password (empty when using DHCP). |
| GET | `/api/v1/wan/ip/stats` | 200 | `wan_stats()` | Total WAN rx/tx byte and packet counters (bytes, packets, errors, discards, unicast/multicast/broadcast packet counts), downstream of every wired and wireless client on the gateway and all mesh extenders. Not seen in the captured HARs — found by testing directly against the router; the counters correctly reflect traffic from extender-connected clients that `wifi_stats()` cannot see. |

## WiFi / mesh

| Method | Path | Status | Used by | Description |
|---|---|---|---|---|
| GET | `/api/v2/home` | 200 | — | Seen. Per-band SSID summary: SSID name, security protocol (e.g. `WPA2_WPA3_PERSONAL`), **plaintext password**, up/down status, max bitrate, and SSID type (`Primary`/`Guest`) per radio (`2_4GHZ`/`5GHZ`/`6GHZ`). |
| GET | `/api/v1/wireless/bandsteering` | 200 | — | Seen. Returns `{"BandSteeringEnable": true/false}`. |
| GET | `/api/v2/wireless/stats/{band}` | 200 (guest), **400** (backhaul) | `wifi_stats()` | Per-SSID traffic counters (status, max bitrate, rx/tx bytes/packets/errors/discards). `{band}` is a path segment, not a query param. Confirmed codes: `24`/`5`/`6` for the primary SSID per band (used by `wifi_stats()`); `guest24`/`guest5`/`guest6` for the guest SSID (returns real counters, unused by the module); `backhaul24`/`backhaul5`/`backhaul6` for the mesh backhaul SSID — always returns 400 on this firmware, so backhaul traffic cannot be read via this endpoint at all. Only ever reports the **gateway's own** radios — traffic on an extender's own radio, and gateway↔extender backhaul traffic, never shows up here (use `wan_stats()` for that). |
| GET | `/api/v4/easymesh/meshdevices` | 200 | `connected_extenders()`, `connected_devices()` | The mesh topology: one entry per mesh node (gateway + extenders) with hostname, model, serial number, firmware (`softwareVersion`), uptime, IPv4, backhaul link info (`Ethernet` with `speed`, or `Wi-Fi` with per-band `wifiLinks` incl. channel/quality/RSSI), plus nested `wifiRadios` → `ssids` → `stations` (wireless clients: hostname, IP, MAC, band, signal strength, link quality) and `ethernetPorts` → `neighbours` (wired clients: hostname, IP, MAC, port speed). This single endpoint backs both `connected_extenders()` and `connected_devices()`. |

## Hosts / DHCP

| Method | Path | Status | Used by | Description |
|---|---|---|---|---|
| GET | `/api/v1/hosts` | 200 | — | Seen. Full known-host list (not just currently-connected devices): device type, mesh device id, MAC, interface, DHCP lease time, link state, hostname/friendly name, IPv6 addresses, active flag, internal id. Broader/lower-level than the mesh-derived `connected_devices()`. |
| GET | `/api/v1/dhcp` | 200 | — | Seen. DHCP server config: enabled state, address pool (`minaddress`/`maxaddress`), lease time, router IP, subnet mask, plus a `reservedpools` list (static DHCP reservations). |

## Firewall / NAT / port forwarding

| Method | Path | Status | Used by | Description |
|---|---|---|---|---|
| GET | `/api/v2/firewall` | 200 | `firewall_settings()` | Global firewall config: security `level`, port-scan detection, fragmented-IP-packet blocking, plus a `services_capabilities` list of well-known service/port definitions (FTP, etc.) used by the UI's rule editor. |
| GET | `/api/v2/firewall/chain?chain=Custom` | 200 | `firewall_settings()` | The "Custom" firewall chain: enabled flag, default policy (`Accept`/`Drop`), and the list of user-defined rules (alias, description, enable, action, protocol, src/dst IP, ports, and interface, IPv4 vs IPv6). |
| GET | `/api/v1/firewall/portblacklist` | 200 | — | Seen. Comma-separated list of ports blocked outright regardless of other rules, e.g. `5060,5061,7547,68,443,8022`. |
| GET | `/api/v1/nat/rules` | 200 | — | Seen. Port-forwarding (NAT) rules: enabled flag plus a `rules` list (empty in captures — no port forwards configured at capture time). |
| GET | `/api/v1/upnp/igd` | 200 | — | Seen. UPnP Internet Gateway Device settings: link state, enabled flag, device UUID, SSDP advertisement interval/TTL. |

## Voice (VoIP)

| Method | Path | Status | Used by | Description |
|---|---|---|---|---|
| GET | `/api/v1/voice/info` | 200 | — | Seen. Voice module type (`modelVoiceType`, e.g. `TR104v1`) and a per-line active flag list. Both lines inactive in captures (no VoIP subscription on this connection). |
| GET | `/api/v1/voice/v1/lines` | 200 | — | Seen. Detailed per-line VoIP config/state: profiles → lines, each with id, enabled flag, display name, call state, call-waiting flag, and more. |

## Not yet implemented in `sagemcom5598.py`

Everything marked **Seen** above has a confirmed 200 response and known
shape but no corresponding method/CLI flag yet: `/api/v1/authenticated`,
`/api/v1/session-count`, `/api/v1/session-timeout`, `/api/v1/user`,
`/api/v1/device/features`, `/api/v1/wan/status`, `/api/v1/wan/ipv4`,
`/api/v2/home`, `/api/v1/wireless/bandsteering`,
`/api/v2/wireless/stats/guest{24,5,6}`, `/api/v1/hosts`, `/api/v1/dhcp`,
`/api/v1/firewall/portblacklist`, `/api/v1/nat/rules`, `/api/v1/upnp/igd`,
`/api/v1/voice/info`, `/api/v1/voice/v1/lines`.

## Confirmed non-working on this firmware

- `GET /api/v1/ui/language` — always 400.
- `GET /api/v2/wireless/stats/backhaul{24,5,6}` — always 400; there is no
  working way to read mesh backhaul traffic counters through this API.
