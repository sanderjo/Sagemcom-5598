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
pip install requests
```

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

Optional overrides: `--ip` (default `192.168.1.254`), `--username` (default `beheer`).

```bash
python3 sagemcom5598.py --login "loginpassword" --connected_devices --firewall_settings
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
