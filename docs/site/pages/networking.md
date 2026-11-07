title: Networking
lede: tOS has its own network stack, written from scratch, with IPv4 and IPv6, TCP and UDP, TLS and DNS, and drivers for the common virtual network cards. This page explains how to connect, what is available, and how programs use it.
---
## Connecting

In a virtual machine the network usually works by itself. In QEMU, start the machine with a network card:

```
qemu-system-x86_64 -cdrom tOS.iso -m 2048 -vga std -nic user,model=e1000
```

tOS finds the card, asks for an address with **DHCP**, and learns the name servers. `ifconfig` shows the result: the card and its driver, its MAC, IPv4 and IPv6 addresses and the counters. `ping <host>` checks that you can reach something.

```
ifconfig
ping 1.1.1.1
ping example.com
```

The **Network** app and the **Network** pane in Settings show the same in a window: see [Activity, Network and Disk Utility](/tos/system-apps).

## Network cards

| Card | Notes |
|---|---|
| virtio-net | The fastest in virtual machines |
| Intel E1000 | QEMU's `model=e1000`; also VirtualBox and VMware |
| Realtek RTL8139 | |
| AMD PCnet (PCnet32, Am79C970A) | QEMU's default |
| NE2000 (RTL8029) | |

They are found by scanning the PCI bus, so nothing needs configuring.

## What the stack speaks

| Layer | Protocols |
|---|---|
| Link | Ethernet, ARP, VLAN (802.1Q), software bridges, bonding |
| Network | IPv4 and IPv6 (with neighbour discovery), ICMP and ICMPv6, routing with policy rules, a stateful packet filter with connection tracking and NAT |
| Transport | TCP, UDP, UDP-Lite, SCTP, DCCP |
| Tunnels and security | TLS 1.2, IPsec headers (AH and ESP), GRE, IP-in-IP, and a WireGuard-style encrypted UDP tunnel |
| Application | DHCP, DNS, HTTP and HTTPS |

The tools to look at and configure them are on [Network tools](/tos/network-tools).

## IPv6

tOS makes a link-local IPv6 address from the card's MAC address at start, and answers neighbour solicitations. `ip6addr` shows it and `ping6 <address>` pings an IPv6 address.

## Name lookup

DNS names are looked up through the name servers that DHCP gave. Programs use the usual functions (`getaddrinfo` and friends), and tOS's own tools (`ping`, `wget`, tFire) do the same.

## Using the network from programs

Linux programs use ordinary sockets, so what they need works as they expect: TCP and UDP, local sockets, `poll`, `select` and `epoll`, loopback (`127.0.0.1`), name lookup.

| To do this | Use |
|---|---|
| Download a file | `wget <url>` (built in, HTTP and HTTPS), or `curl`, or GNU `wget` from the package |
| Talk to another computer | `ssh` (package `openssh`) |
| Copy files | `rsync`, `git` |
| Run a web or other server | Any Linux server program that uses sockets, for example Python's `http.server` |
| Fetch from a script | `tos.http_get(url)` in MicroPython (HTTP only) |

## SSH

The `openssh` package brings the OpenSSH client and server. You can log in to a tOS machine over SSH and get a real terminal with job control; running `sshd` needs the usual host keys and a configuration. See [Git and SSH](/tos/dev-git).

## HTTPS and certificates

The kernel and tFire do their own TLS 1.2, and check the server's certificate against the list of trusted roots in tOS. `wget -k https://...` (the built-in one) skips the check, and `tls verify off` turns checking off for the session; use them only for a server you know.

Linux programs use the `ca-certificates` package: Python, curl and GNU wget are set up to find it. If a program says it cannot verify a certificate, install it (it comes along with the packages that need it).

## Firewall and NAT

tOS has a stateful packet filter with connection tracking, and source and destination NAT. Rules are added with the `fw` command: see [Network tools](/tos/network-tools).

## The browser

**tFire** opens pages over HTTPS and HTTP: see [tFire](/tos/tfire).

## When it does not work

See [Troubleshooting](/tos/troubleshooting): no card, no address, no name lookup, and certificate errors.
