title: Network tools
lede: The shell commands that look at and configure the network: ping and addresses, routes and policies, the firewall and NAT, VLANs, bridges, bonds, tunnels, and the less common protocols.
---
These commands are built into the shell (some need root). Run `man <command>` for details on your system.

## Looking and testing

| Command | What it does |
|---|---|
| `ifconfig` | The card, its driver, MAC and IP addresses, and packet counters |
| `ping <host>` | ICMP ping (IPv4), with a name or an address |
| `ping6 <addr>` | ICMPv6 echo (IPv6) |
| `ip6addr` | The link-local IPv6 address |
| `wget <url>` | Download over HTTP |
| `wget [-k] https://<url>` | Download over HTTPS (TLS 1.2); follows redirects; `-k` skips the certificate check |
| `ws <ws://host/path> [msg]` | Open a WebSocket, optionally send a message, and print what arrives |
| `tls verify on\|off` | Turn certificate checking on or off |
| `tls trust <file>` | Trust extra root certificates (PEM or DER) until restart |

The **Network** app shows interfaces, connections, routes and firewall rules live.

## Routing

tOS has static routes, 8 independent routing tables and policy rules that pick a table by source and destination.

| Command | What it does |
|---|---|
| `route show` | The routing table |
| `route add <dst/pfx> <gw> [metric] [table]` | Add a static route |
| `route del <dst/pfx> [table]` | Delete a route |
| `policy show` | The policy rules |
| `policy add <src/pfx> <dst/pfx> <table> <prio>` | Add a policy rule |
| `policy del <prio>` | Delete a rule by priority |

## Firewall and NAT

A stateful packet filter with a first-match rule list, a connection-tracking table, and source and destination NAT.

| Command | What it does |
|---|---|
| `fw rule add <proto> <src/pfx> <dst/pfx> [dport] <ACCEPT\|DROP\|REJECT>` | Add a rule |
| `fw rule del <idx>` | Delete a rule |
| `fw rule list` | List the rules |
| `fw nat add SNAT\|DNAT <match/pfx> [port] <new_ip> [new_port]` | Add a NAT rule |
| `fw nat del <idx>`, `fw nat list` | Delete and list NAT rules |
| `fw ct` | Show the connection-tracking table |

## VLANs, bridges and bonds

| Command | What it does |
|---|---|
| `vlan add <vid>`, `vlan rm <vid>`, `vlan list` | The 802.1Q VLAN IDs allowed in |
| `bridge create <name>`, `bridge del <name>` | Make and delete a software bridge |
| `bridge addif <br> <if>`, `bridge delif <br> <if>` | Add and remove a member |
| `bridge list` | Bridges and their forwarding table |
| `bond create <name> [failover\|balance]`, `bond del <name>` | Make and delete a link-aggregation bond |
| `bond addif <bond> <if>`, `bond delif <bond> <if>` | Add and remove a slave |
| `bond failover <name>` | Switch to the next slave by hand |
| `bond list` | Bonds and the active slave |

## Tunnels

| Command | What it does |
|---|---|
| `gre add <local> <remote> [key_hex]`, `gre del <idx>`, `gre list` | GRE tunnels |
| `ipip add <local> <remote>`, `ipip del <idx>`, `ipip list` | IP-in-IP tunnels |
| `wg add <remote_ip> <rport> <lport> <psk_hex64> <peer_id_hex>`, `wg del <idx>`, `wg list` | A WireGuard-style encrypted UDP tunnel (XChaCha20-Poly1305, pre-shared key) |
| `ipsec add ah <peer-ip> <spi-hex> <key-hex>` | Add an IPsec AH security association |
| `ipsec_sa` | Show the IPsec security associations |

## Other protocols

| Command | What it does |
|---|---|
| `sctp_connect <ip> <port>`, `sctp_send <data>`, `sctp_close` | An SCTP association |
| `dccp_connect <ip> <port>`, `dccp_send <data>` | A DCCP connection |
| `udplite_send <ip> <port> <data> [coverage]` | A UDP-Lite datagram; `coverage` is the number of bytes the checksum covers (0 is all, 8 is the header only) |
| `ipx send <node12hex> <data>` | An IPX datagram, a historical protocol kept for study |

## Where the code is

The network stack lives in `kernel/net/` in the source: one file per protocol (`arp.c`, `ip.c`, `tcp.c`, `udp.c`, `sctp.c`, `dccp.c`, `route.c`, `fw.c`, `ipsec.c`, `wgtun.c`, and so on) and one per network card driver. See [How tOS works inside](/tos/architecture).
