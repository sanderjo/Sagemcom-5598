# sagemcom5598

Python client and CLI for the Sagemcom F@st 5598 router (as provided by Delta
Fiber in the Netherlands). Talks directly to the router's REST/JSON web API
over plain HTTP — no Selenium, no browser emulation.

The API is undocumented; the login flow and endpoints were reverse-engineered
from HAR captures of the router's own web UI and its Angular JS bundle.

## Requirements

- Python 3.10+
- [`requests`](https://pypi.org/project/requests/)
- [`mcp`](https://pypi.org/project/mcp/) 2.x (only for the MCP server)

```bash
pip install -r requirements.txt
```

## Files

| File                       | Purpose                                                   |
|----------------------------|------------------------------------------------------------|
| `sagemcom5598.py`          | The module and CLI                                          |
| `wifi_stats_diff.py`       | Diffs two `wifi_stats`/`wan_stats` snapshots into MB sent/received |
| `sagemcom5598_mcp.py`      | MCP server: lets an AI assistant inspect and troubleshoot the router |
| `sagemcom5598_diagnose.py` | The checks behind the MCP server's `diagnose` tool              |
| `sagemcom5598_history.py`  | Collects periodic snapshots into SQLite (`history.db`)          |
| `requirements.txt`         | Python dependencies                                          |
| `credentials.ini.example`  | Template for `credentials.ini` (copy it, fill in your password) |
| `nicknames.txt`            | Optional friendly names for extenders/clients (see below)    |
| `README.md`                | This file                                                   |
| `LICENSE`                  | GPL-3.0 license text                                        |
| `tests/test_sagemcom5598.py` | Live integration tests against a real router               |
| `tests/test_diagnose.py`   | Offline tests for the diagnosis logic                        |
| `tests/requirements.txt`   | Dependencies for running the tests                           |

## Usage as a module

```python
from sagemcom5598 import Sagemcom5598

client = Sagemcom5598()
client.login(password="your-router-password")  # ip/login default to 192.168.1.254 / beheer

client.connected_extenders()
client.connected_devices()
client.topology()
client.firewall_settings()

client.logout()
```

All getters return plain `dict`/`list` structures, ready for `json.dumps()`.

### `login(ip="192.168.1.254", login="beheer", password=None)`

Authenticates against the router using its salted challenge-response scheme
(SHA-512-crypt of the password, mixed with a server nonce and a client
cnonce). Raises `requests.HTTPError` on a rejected login.

### `device_info()`

Router identity info from the unauthenticated `/api/v1/open` endpoint
(reachable even before login). `uptime` here is the device's own uptime
since last reboot — what the GUI's #/mybox/deviceInfo/general page shows —
not the WAN link's uptime (see `wan_ipv4()` for that).

```json
{
  "uptime": 865478,
  "serial_number": "N725115C6000105",
  "firmware": "SGQA530011400P",
  "wan_status": "Up",
  "wan_ipv4": "100.64.193.104"
}
```

### `connected_extenders()`

Mesh extenders and their firmware version. Source: `#/wifi/2.4GHz/priv/mesh/extenders`.

```json
[
  {
    "hostname": "F381D-N725150C5001524",
    "model": "FAST381",
    "serial_number": "N725150C5001524",
    "firmware": "SG_W7EXT_DELTA_MLOvDrop5.5_104",
    "ipv4": "192.168.1.10",
    "uptime": "184616",
    "parent": "mygateway",
    "backhaul": {"linkType": "Ethernet", "rootDeviceId": "ec:fc:2f:47:e7:74", "speed": 1000},
    "signal_strength_dbm": {}
  }
]
```

`uptime` is raw seconds as a string; the CLI table formats it as `Xd
HH:MM:SS` (or just `HH:MM:SS` under a day).

`parent` is the hostname of whatever this extender's backhaul actually
connects to — the gateway, or (in a multi-hop mesh) another extender —
resolved from `backhaul.rootDeviceId` against every mesh node's `deviceId`.
This is the same parent/child data behind the topology picture at
`#/wifi/2.4GHz/priv/mesh/overview`; chaining `parent` across extenders
reconstructs that whole tree, e.g. extender `-525`'s `parent` is `-524`'s
hostname, and `-524`'s `parent` is `"mygateway"`.

`backhaul.linkType` is `"Ethernet"` when the extender is wired to the gateway,
or `"Wi-Fi"` when it's wireless — in which case there's a `wifiLinks` array
instead of `speed`, with one entry per band:

```json
"backhaul": {
  "linkType": "Wi-Fi",
  "rootDeviceId": "ec:fc:2f:47:e7:74",
  "wifiLinks": [
    {"band": "2.4", "channel": "1", "linkQuality": "GOOD", "signalStrength": -55},
    {"band": "5", "channel": "100", "linkQuality": "GOOD", "signalStrength": -54},
    {"band": "6", "channel": "5", "linkQuality": "MEDIUM", "signalStrength": -61}
  ]
}
```

With MLO enabled, all three bands can be in simultaneous use as one
aggregated backhaul link, so `wifiLinks` isn't "candidates to pick from" —
it's signal quality per band of a link that may be using all of them at
once. See the note under `wifi_stats()` for why none of that traffic is
visible through this router's wifi stats API either way.

`signal_strength_dbm` is a flattened convenience view of the same data —
`{"2.4": -55, "5": -54, "6": -61}` for a Wi-Fi backhaul extender, `{}` for
an Ethernet one. A band the extender isn't actually using for backhaul
still shows up here (the router always reports `wifiLinks` for all three
bands), typically as `0` dBm with `linkQuality: "BAD"` in the raw
`backhaul` data — check `backhaul.wifiLinks` if you need `linkQuality` or
`channel` too.

### `connected_devices()`

All devices connected to the gateway or its extenders, wired and wireless.
Source: `#/wifi/2.4GHz/priv/mesh/devices`.

```json
[
  {
    "name": "raspizero",
    "ip": "192.168.1.157",
    "mac": "90:de:80:05:2c:d9",
    "connection": "wireless",
    "band": "2.4",
    "signal_strength": -71,
    "ssid": "my-ssid",
    "link_quality": "MEDIUM",
    "connected_via": "mygateway"
  },
  {
    "name": "brixit",
    "ip": "192.168.1.252",
    "mac": "e0:d5:5e:c2:a4:6a",
    "connection": "wired",
    "band": null,
    "signal_strength": null,
    "ssid": null,
    "link_quality": null,
    "connected_via": "F381D-N725150C5001524",
    "link_speed_mbps": 1000
  }
]
```

Wired devices have `band`, `signal_strength`, `ssid` and `link_quality` set
to `null`, and carry a `link_speed_mbps` (switch port speed) instead.

### `topology()`

The mesh as a tree: the gateway at the root, each extender nested under
whatever it actually backhauls through (gateway, or another extender in a
multi-hop mesh — see `parent` in `connected_extenders()`), with every
node's directly-connected clients attached to it. Combines
`connected_extenders()` and `connected_devices()` into one structure.
Source: `#/wifi/2.4GHz/priv/mesh/overview`.

```json
{
  "hostname": "mygateway",
  "ipv4": "192.168.1.254",
  "signal_strength_dbm": null,
  "clients": [{"...": "one entry per connected_devices() item, connected_via this node"}],
  "extenders": [
    {
      "hostname": "F381D-N725150C5001524",
      "ipv4": "192.168.1.10",
      "signal_strength_dbm": {"2.4": -48, "5": -51, "6": -60},
      "clients": ["..."],
      "extenders": [
        {
          "hostname": "F381D-N725150C5001525",
          "ipv4": "192.168.1.11",
          "signal_strength_dbm": {"2.4": 0, "5": -38, "6": 0},
          "clients": ["..."],
          "extenders": []
        }
      ]
    }
  ]
}
```

The CLI (`--topology`) renders this as ASCII art instead:

```
mygateway (192.168.1.254)
|
+-- raspizero                  192.168.1.157   wireless 2.4GHz  -67 dBm
+-- nanopineo2                 192.168.1.109   wired
|
+-- F381D-N725150C5001524      192.168.1.10    extender  backhaul 2.4=-48  5=-51  6=-60 dBm
    |
    +-- brixit                     192.168.1.252   wired
    |
    +-- F381D-N725150C5001525      192.168.1.11    extender  backhaul 2.4=0  5=-38  6=0 dBm
        |
        +-- LT001608                   192.168.1.237   wireless 5GHz  -61 dBm
```

### `firewall_settings()`

General firewall settings plus the custom rule chain, including whether each
rule applies to IPv4 or IPv6. Source: `#/access-control/firewall/custom`.

```json
{
  "level": 2,
  "port_scan_detection": false,
  "block_fragmented_ip_packets": false,
  "custom_chain_enabled": true,
  "default_policy": "Drop",
  "rules": [
    {
      "id": 1,
      "alias": "cpe-1",
      "description": "ipv6 8080 come in",
      "enabled": true,
      "action": "Accept",
      "ip_version": "ipv6",
      "protocol": "tcp",
      "src_ip": "",
      "src_ports": 8080,
      "src_interface": "lan",
      "dst_ip": "",
      "dst_ports": -1,
      "dst_interface": "wan"
    }
  ]
}
```

### `wifi_stats()`

Traffic stats for each wifi band. Source: `#/wifi/5GHz/priv/stats`.

```json
{
  "2.4": {
    "status": "Up",
    "max_bitrate_mbps": 344,
    "rx": {"bytes": 402049107, "packets": 1560891, "packetsbroadcast": 0, "packetsunicast": 0, "packetsmulticast": 101684, "packetserrors": 0, "packetsdiscards": 0},
    "tx": {"bytes": 8453816484, "packets": 9325830, "packetsbroadcast": 0, "packetsunicast": 0, "packetsmulticast": 3292118, "packetserrors": 61841, "packetsdiscards": 0}
  },
  "5": { "...": "same shape" },
  "6": { "...": "same shape" }
}
```

**Note: this only reports the gateway's own primary SSID, per band.** It does
not see:
- traffic on an extender's own radio (a client connected to the extender
  never touches the gateway's radio at all), or
- backhaul traffic between the gateway and an extender, wired or wireless.

Wireless backhaul between a gateway and its extenders uses a separate,
hidden SSID named `<default-ssid-prefix>_BH` (e.g. `DELTA-47e774_BH` — the
suffix is derived from the gateway's own MAC address). It's broadcast by
every mesh node on every band, has no associated client "stations", and
never shows up in the UI or in `connected_devices()`. The endpoint behind
`wifi_stats()` does support querying other SSID types per band — a `guest24`/
`guest5`/`guest6` variant exists and returns real counters — but the
equivalent `backhaul5`/`backhaul24`/`backhaul6` codes all return `400`, so
there's no way to read backhaul byte counts from this API at all, on this
firmware. With MLO enabled, backhaul can use all three bands simultaneously
as a single aggregated link, so there isn't even a single "the backhaul
band" to point `wifi_stats()` at.

Practically: if a device is connected to an extender (wired or wireless),
`wifi_stats()` will show little to nothing for its traffic, no matter how
large the transfer — confirmed by downloading 10GB on two different
extender-connected devices and seeing under 100MB combined movement across
all three of the gateway's bands. Use `wan_stats()` instead if you want
total traffic regardless of which node or band actually carried it — it
sits downstream of all of this and reflects real activity accurately (the
same 10GB download showed up there as an ~11.3GB rx delta).

### `wan_stats()`

Total bytes in/out on the WAN link — unlike `wifi_stats()`, this includes
traffic from every client on the network, wired or wireless, on the gateway
or on any extender, since it's counted downstream of all of them.

```json
{
  "rx": {"bytes": 1196748307897, "packets": 943569284, "packetserrors": 0, "packetsdiscards": 0, "unicastpackets": 928017696, "multicastpackets": 15551588, "broadcastpackets": 0},
  "tx": {"bytes": 304520657964, "packets": 386150799, "packetserrors": 0, "packetsdiscards": 0, "unicastpackets": 386150667, "multicastpackets": 123, "broadcastpackets": 9}
}
```

### `wan_ipv4()`

WAN link status: connection state, public IP, gateway, uptime, MAC address —
this is what the GUI's "IP Address" / "Status" fields show.

```json
{
  "status": "Up",
  "addressing_type": "DHCP",
  "address": "100.64.193.104",
  "subnet": "255.255.192.0",
  "gateway": "100.64.192.1",
  "uptime": 56112,
  "mac_address": "EC:FC:2F:47:E7:70"
}
```

Note `100.64.0.0/10` is [CGNAT](https://en.wikipedia.org/wiki/Carrier-grade_NAT)
space, not a public routable address — expected if your ISP shares IPv4
addresses across subscribers. `--firewall_allow_ipv6_port` won't help make a
service reachable from outside in that case, since there's no public IPv4 to
forward to.

### More getters

| Method            | Returns |
|-------------------|---------|
| `gateway_node()`  | The gateway's mesh entry: hostname, `device_id` (the MAC extenders use as `rootDeviceId`), model, serial, firmware |
| `wan_status()`    | WAN link state and `last_change` (seconds since it last went up/down) |
| `lan_ports()`     | Physical Ethernet ports (WAN port included, see `role`): status, negotiated `speed_mbps`, duplex, rx/tx counters |
| `hosts()`         | Every host the router knows, including offline ones (`active`), with IPv4/IPv6, lease, last seen |
| `dhcp()`          | LAN DHCP server config: pool, lease time, router IP, reserved pools |
| `ntp()`           | Clock sync status, time zone, NTP servers |
| `wifi_config()`   | SSIDs per band (status, security - **not** their passwords), channel per band per mesh node, band steering, MLO |
| `event_log()`     | The router's event log, oldest first (see below) |
| `device_log()`    | The same log raw, exactly as `/api/v1/device/log` returns it (all fields, no timestamp correction) |

`event_log()` returns the router's own log (`/api/v1/device/log`) - a ring
buffer of a few thousand entries, about a week on a typical home network:

```json
{"time": "2026-09-24T21:27:48+02:00", "level": "info", "module": "WETH",
 "message": "Wan Ethernet connectivity has been disconnected", "clock_corrected": false}
```

Modules seen: `WIFI` (connects, disconnects, failed wifi logins - including
the hidden mesh backhaul SSIDs `WL_BACKHAUL_*`), `SYS` (boot + boot reason,
TR-069 sessions with the ISP), `GUI` (admin logins/logouts with source IP),
`WETH`/`LETH` (WAN/LAN Ethernet link up/down), `DHCPC` (WAN DHCP client),
`DHCPS`, `DNS`. Entries logged right after boot, before NTP sync, carry a
bogus `2013-01-01` timestamp; `event_log()` shifts those to the real boot
time and sets `clock_corrected`.

### `logout()`

Ends the router session. The router only allows one authenticated LAN admin
session at a time, so call this when you're done.

## Usage from the CLI

```bash
python3 sagemcom5598.py --login "loginpassword"
```

On success, prints `Login OK` followed by identity/status info (from
`device_info()`):

```
Login OK
Serial number: N725115C6000105
Software version: SGQA530011400P
Device uptime: 10d 00:24:33
WAN IPv4: 100.64.193.104
```

On failure, prints `Login failed: <reason>` (wrong password, or no router
found at the given IP) with a non-zero exit code.

Optional flags, each printed in a human-readable table:

| Flag                    | Shows                          |
|-------------------------|---------------------------------|
| `--connected_extenders` | Mesh extenders, firmware, uptime, mesh parent, backhaul type (`ethernet`/`wifi`) and backhaul signal strength per band (`-` = band not used) |
| `--connected_devices`   | Wired and wireless clients      |
| `--topology`            | Mesh + clients as ASCII art (not a table — see `topology()` above) |
| `--firewall_settings`   | Firewall config and custom rules|
| `--wifi_stats`          | Traffic stats per wifi band (rx/tx in MB, 1 MB = 1024*1024 bytes) |
| `--wan_stats`           | Total WAN rx/tx (MB, 1 MB = 1024*1024 bytes) |
| `--wan_ipv4`            | WAN link status, public IP, gateway, uptime |
| `--device-log`          | The router's full event log (`/api/v1/device/log`) as-is: every entry with all its fields (`date`, `log`, `module`, `flags`, `param`), oldest first, timestamps uncorrected |

`--firewall_allow_ipv6_port PORT` is a write action, not a table: it adds two
Custom-chain firewall rules (one per direction) that Accept ipv6 tcp/udp
traffic on `PORT`, and prints `Allowed ipv6 port PORT` on success.
`--firewall_remove_ipv6_port PORT` undoes that: it removes every ipv6
Custom-chain rule allowing `PORT` and prints how many rules were removed.

Optional overrides: `--ip` (default `192.168.1.254`), `--username` (default `beheer`).

```bash
python3 sagemcom5598.py --login "loginpassword" --connected_devices --firewall_settings
```

### Storing credentials in `credentials.ini`

Instead of passing `--login` every time, copy `credentials.ini.example` to
`credentials.ini` (next to `sagemcom5598.py`) and fill in your password:

```ini
[router]
ip = 192.168.1.254
login = beheer
password = your-router-password
```

Then just run:

```bash
python3 sagemcom5598.py --connected_devices
```

Any of `--login`, `--ip`, or `--username` passed on the command line takes
precedence over the values in `credentials.ini`. This file is gitignored —
never commit it.

### Friendly names in `nicknames.txt`

Extender hostnames like `F381D-N725150C5021992` are hard to recognise. Put a
`nicknames.txt` next to `sagemcom5598.py` with one `<hostname or MAC> <nickname>`
per line (the nickname may contain spaces, `#` starts a comment):

```
F381D-N725150C5019024 BOL.com
F381D-N725150C5021992 schuur
```

The CLI then shows the nickname next to the technical name: `--topology`
(`F381D-N725150C5021992 (schuur)`), a `nickname` column and the parent in
`--connected_extenders`, and `connected_via` in `--connected_devices`. The MCP
server adds `nickname` (or `parent_nickname`, `connected_via_nickname`,
`via_nickname`) fields next to every matching name in its results, and its
`device` arguments accept a nickname. The file is re-read on every use, so
edits apply without restarting anything. `topology()`/`connected_extenders()`
themselves stay unchanged; use `load_nicknames()` and `add_nicknames()` to get
the same in your own code.

## Measuring traffic with `wifi_stats_diff.py`

Capture two `--wifi_stats` or `--wan_stats` snapshots, some time apart, and
diff them to see how many MB were sent/received in between:

```bash
python3 sagemcom5598.py --wifi_stats > wifi_stats.before.txt

# ... wait, e.g. run a speed test or a big download ...

python3 sagemcom5598.py --wifi_stats > wifi_stats.after.txt

python3 wifi_stats_diff.py
```

```
band           rx MB     tx MB  notes
2.4            10.44    607.69  
5              88.31  13968.58  
6              77.29      5.48 
```

`wifi_stats_diff.py` auto-detects which kind of table it's looking at (`--wifi_stats`,
keyed by band, or `--wan_stats`, a single `wan` row) and diffs accordingly. It
defaults to `wifi_stats.before.txt`/`wifi_stats.after.txt`, or takes two
explicit file paths:

```bash
python3 sagemcom5598.py --wan_stats > wan_stats.before.txt
# ... wait ...
python3 sagemcom5598.py --wan_stats > wan_stats.after.txt
python3 wifi_stats_diff.py wan_stats.before.txt wan_stats.after.txt
```

A negative delta (e.g. after a router reboot resets the counters) is flagged
as a counter reset rather than silently shown as negative MB.

**If you want to measure traffic to/from a device connected to an extender**
(wired or wireless), use `--wan_stats`, not `--wifi_stats`: the wifi stats
endpoint only covers the gateway's own radios, so extender and backhaul
traffic never shows up there, no matter how large the transfer.

## MCP server for AI assistants

`sagemcom5598_mcp.py` is an [MCP](https://modelcontextprotocol.io) server, so an
AI assistant (Claude Code, Claude Desktop, ...) can answer questions like
*"why is the wifi upstairs slow?"* or *"did the internet drop last night?"* by
looking at the router itself. Aimed at the owner of the router as well as an
ISP helpdesk employee going through a customer's setup.

It is **read-only**: no tool changes settings (the firewall write actions are
deliberately not exposed), and secrets the router hands out (wifi passwords,
PPP/PLOAM credentials) are never returned.

### Tools

| Tool               | What it answers |
|--------------------|-----------------|
| `diagnose(hours=25)` | Runs all checks and returns findings sorted by severity, each with evidence, likely root cause and suggested fix. Start here. |
| `router_overview`  | Model, serial, firmware, uptime, WAN, clock sync, extenders, client counts |
| `network_topology` | Gateway → extenders → clients tree |
| `list_devices(include_inactive)` | Clients with IP/MAC/band/signal/node; optionally also offline known hosts |
| `list_extenders`   | Extenders with firmware, uptime, parent, backhaul detail |
| `wan_details`      | WAN status, IP (CGNAT flag), time since last change, WAN port speed, counters |
| `ethernet_ports`   | Gateway Ethernet ports: speed, duplex, errors |
| `wifi_details`     | SSIDs (no passwords), channels per node, band steering, MLO, radio stats |
| `firewall_details` | Firewall level and custom rules |
| `dhcp_details`     | DHCP server config and all known hosts |
| `event_log(hours, module, level, device, contains, limit)` | Router log, filtered, MACs annotated with device names |
| `event_summary(hours)` | Log condensed: counts per event kind and per wifi device, GUI logins by source |
| `history_summary(hours)` | From collected snapshots: reboots, firmware/WAN IP/mesh/port changes, hourly throughput and client counts, backhaul signal ranges, per-client presence/signal/roaming |
| `device_history(device, hours)` | Snapshot timeline for one client or extender |

`diagnose` currently checks for: WAN down / WAN drops (with duration) / DHCP
lease trouble / CGNAT; gateway reboots with the logged boot reason (power,
watchdog, software); extender reboots, and whether they happened together
(firmware push, shared power outage) or alone; mesh nodes and clients failing
wifi authentication; clients and mesh backhaul links that keep reconnecting;
weak client signal; weak or 2.4 GHz-only extender backhaul and multi-hop
wireless chains; mixed extender firmware; Ethernet links below 1 Gbps, half
duplex, port errors; high wifi transmit error ratios; clock not synced;
plus reboots/changes and collection gaps found in history.

### History: the last 25 hours

The router's event log already goes back about a week, but the router keeps
no *measurements*: signal strengths, throughput and uptimes only exist as
"right now". So the MCP server takes a snapshot every 5 minutes while it runs
(`--collect-interval`, `0` disables) into `history.db` (SQLite, pruned to
`--retention` hours, default 25).

An MCP server only runs while the assistant is open. For history around the
clock, run the collector separately as well (cron, systemd, a Raspberry Pi):

```bash
python3 sagemcom5598_history.py --interval 300          # loop forever
python3 sagemcom5598_history.py                          # one snapshot (for cron)
```

Both write to the same `history.db`; the MCP server then reads what the
collector gathered.

### Setup

```bash
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
cp credentials.ini.example credentials.ini   # fill in the router password
```

Credentials come from `credentials.ini`, or from the environment variables
`SAGEMCOM_IP`, `SAGEMCOM_LOGIN`, `SAGEMCOM_PASSWORD` (which win). The history
file can be moved with `SAGEMCOM_HISTORY_DB` or `--db`.

Register with Claude Code:

```bash
claude mcp add sagemcom5598 -- /path/to/.venv/bin/python /path/to/sagemcom5598_mcp.py
# or with the password in the environment instead of credentials.ini:
claude mcp add sagemcom5598 -e SAGEMCOM_PASSWORD=... -- /path/to/.venv/bin/python /path/to/sagemcom5598_mcp.py
```

Use the venv's Python, not plain `python3`: a Python without the `mcp`
package makes the server exit at once (`claude mcp list` then shows
"Failed to connect"). Without `-s user` the server is only available when
Claude Code is started in this directory.

For Claude Desktop, add to `claude_desktop_config.json`:

```json
{
  "mcpServers": {
    "sagemcom5598": {
      "command": "/path/to/.venv/bin/python",
      "args": ["/path/to/sagemcom5598_mcp.py"]
    }
  }
}
```

Then just ask, e.g. *"Diagnose my Sagemcom router"*, *"Which devices
dropped off wifi most today?"*, *"Was there an internet outage last night?"*.

Keep in mind: the router allows one admin session at a time. Every tool call
(and every history snapshot) logs in and out, so someone logged into the web
GUI at the same moment may get logged out. These logins also show up in the
router's own log as GUI logins from this machine's IP - `event_summary`
marks them as `(this MCP server)`.

## Tests

`tests/test_sagemcom5598.py` are live integration tests that run against a
real router, using the credentials from `credentials.ini`. They're skipped
automatically if `credentials.ini` is missing or the router can't be reached.
`tests/test_diagnose.py` tests the diagnosis logic offline, with fixture data.

```bash
python3 -m unittest discover -s tests -v
```

## Notes

- Tested against a Sagemcom F@st 5598 (model `F5598T`) on Delta Fiber's
  firmware `SGQA530011400P`. Other firmware versions may differ.
- The router uses no session cookies — the authenticated state is tracked
  server-side, apparently keyed to the client connection, and only one LAN
  admin session is allowed at a time.
- This is an unofficial, reverse-engineered client, not affiliated with
  Sagemcom or Delta Fiber.

## License

[GPL-3.0](LICENSE)
