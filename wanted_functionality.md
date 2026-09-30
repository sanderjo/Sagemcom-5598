# Sagemcom 5598 - get and set items

Connected is a Sagemcom 5598 router: 
- 2.5 Gbps WAN interface 
- 2.5 Gbps LAN interface
- a few 1 Gbps LAN interfaces
- wifi 2.4, 5 and 6 Ghz. Tri-band, with MLO
- IPv4 and IPv6
- Firewall and port-forwarding

The goal is a python module that can get and set items in the Sagemcom 5598, based on functionality seen via the GUI.

The router is provided by Delta Fiber in the Netherlands


## login

Login http://192.168.1.254/, with credentials provided by the user in the startup.

## Input

Provided are HAR files from Chrome: CTRL + SHIFT + I.
A typical HAR file is 10-30 MB in size.

## wanted Functions / classes

I want a class "sagemcom5598", with methods:

- login(ip, login, password). Default ip is 192.168.1.254, with access via http. Default login is 'beheer'
- connected_extenders, and their firmwares. Source: http://192.168.1.254/#/wifi/2.4GHz/priv/mesh/extenders . Output in JSON
- connected_devices, from http://192.168.1.254/#/wifi/2.4GHz/priv/mesh/devices . Output: JSON with names, ip, mac, and wifi band and wifi signal strength
- firewall_settings, from http://192.168.1.254/#/access-control/firewall/custom . Output, JSON with settings, including IPV4 or IPv6.
- wifi_stats, from http://192.168.1.254/#/wifi/5GHz/priv/stats . Output in JSON with stats for 2.4, 5 and 6 GHz bands.

The python code should use pure python code, not Selenium or other browser emulators.


## Target audience

Users/developers of the Sagemcom 5598 that want to get and set items in the Sagemcom 5598

## Usage

a file "sagemcom5598.py" that can be used from CLI and as module

### From CLI

`python3 sagemcom5598.py --login "loginpassword"` 

Optional parameters:
`--connected_extenders`: shows in human readable way the info
`--connected_devices`: shows in human readable way the info
`--firewall_settings`: shows in human readable way the info
 
 ### from within python3 scripts
 
 The methods output in JSON format for further usage
 
 



