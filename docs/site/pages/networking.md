title: Networking
lede: tOS has its own network stack, written from scratch, with IPv4 and IPv6, TCP and UDP, TLS and DNS, and drivers for the common virtual network cards.
---
## Network cards

virtio-net, Intel E1000, Realtek RTL8139, AMD PCnet and NE2000 (RTL8029). In QEMU, `-nic user,model=e1000` gives you one with internet access. `ifconfig` shows the card, its addresses and the counters.

## What the stack speaks

| Layer | Protocols |
|---|---|
| Link | Ethernet, ARP, VLAN (802.1Q), software bridges, bonding |
| Network | IPv4 and IPv6 (with neighbour discovery), ICMP and ICMPv6, routing with policy rules, a stateful packet filter with connection tracking and NAT |
| Transport | TCP, UDP, UDP-Lite, SCTP, DCCP |
| Tunnels and security | TLS 1.2, IPsec headers (AH and ESP), GRE, IP-in-IP, and a WireGuard-style encrypted UDP tunnel |
| Application | DHCP, DNS, HTTP and HTTPS |

Linux programs use these through ordinary sockets, so what they need (TCP and UDP, local sockets, `epoll`, name lookup) works as they expect.

## Tools

![The Network app](shot:network.png)

- `ping <host>`, `ifconfig`, and `wget <url>` are built in.
- The **Network** app shows your addresses and the live download and upload speed.
- Packages bring the usual tools: `curl`, `wget` (GNU, with HTTPS), `git`, `rsync`, and OpenSSH.

## SSH

The `openssh` package brings the OpenSSH client and server. You can log in to a tOS machine over SSH and get a real terminal with job control; running `sshd` needs the usual host keys and configuration.

## HTTPS and certificates

The kernel and tFire do their own TLS. Linux programs use the `ca-certificates` package: Python, curl and wget are set up to find it. Install it (it comes along with the packages that need it) if a program says it cannot verify a certificate.

## The browser

![tFire](shot:tfire/home.png)

**tFire** opens pages over HTTPS and HTTP: text, headings, lists, links, pictures, style sheets and JavaScript. Type an address in its bar, or words to search.
