# sagemcom5598

Python client and CLI for the Sagemcom F@st 5598 router (as provided by Delta
Fiber in the Netherlands). Talks directly to the router's REST/JSON web API
over plain HTTP — no Selenium, no browser emulation.

The API is undocumented; the login flow and endpoints were reverse-engineered
from HAR captures of the router's own web UI and its Angular JS bundle.

## Requirements

- Python 3.9+
- [`requests`](https://pypi.org/project/requests/)

```bash
pip install -r requirements.txt
```

## Files

| File                       | Purpose                                                   |
|----------------------------|------------------------------------------------------------|
| `sagemcom5598.py`          | The module and CLI                                          |
| `wifi_stats_diff.py`       | Diffs two `wifi_stats`/`wan_stats` snapshots into MB sent/received |
| `requirements.txt`         | Python dependencies                                          |
| `credentials.ini.example`  | Template for `credentials.ini` (copy it, fill in your password) |
| `README.md`                | This file                                                   |
| `LICENSE`                  | GPL-3.0 license text                                        |
| `tests/test_sagemcom5598.py` | Live integration tests against a real router               |
| `tests/requirements.txt`   | Dependencies for running the tests                           |

## Usage as a module

```python
from sagemcom5598 import Sagemcom5598

client = Sagemcom5598()
client.login(password="your-router-password")  # ip/login default to 192.168.1.254 / beheer

client.connected_extenders()
client.connected_devices()
client.firewall_settings()

client.logout()
```

All getters return plain `dict`/`list` structures, ready for `json.dumps()`.

### `login(ip="192.168.1.254", login="beheer", password=None)`

Authenticates against the router using its salted challenge-response scheme
(SHA-512-crypt of the password, mixed with a server nonce and a client
cnonce). Raises `requests.HTTPError` on a rejected login.

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
    "backhaul": {"linkType": "Ethernet", "rootDeviceId": "ec:fc:2f:47:e7:74", "speed": 1000}
  }
]
```

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

### `logout()`

Ends the router session. The router only allows one authenticated LAN admin
session at a time, so call this when you're done.

## Usage from the CLI

```bash
python3 sagemcom5598.py --login "loginpassword"
```

Prints `Login OK` on success, or `Login failed: <reason>` (wrong password,
or no router found at the given IP) with a non-zero exit code on failure.

Optional flags, each printed in a human-readable table:

| Flag                    | Shows                          |
|-------------------------|---------------------------------|
| `--connected_extenders` | Mesh extenders and firmware     |
| `--connected_devices`   | Wired and wireless clients      |
| `--firewall_settings`   | Firewall config and custom rules|
| `--wifi_stats`          | Traffic stats per wifi band     |
| `--wan_stats`           | Total WAN rx/tx bytes           |

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

## Tests

`tests/test_sagemcom5598.py` are live integration tests that run against a
real router, using the credentials from `credentials.ini`. They're skipped
automatically if `credentials.ini` is missing or the router can't be reached.

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
