# C Software Router

A software router implemented in C on Linux, built to explore
packet processing, routing, forwarding, network security, and
high-performance networking.

## Objectives

- Understand Linux networking APIs
- Capture and parse network packets
- Implement IPv4 packet processing
- Build a routing table
- Implement packet forwarding
- Handle ICMP
- Implement ARP
- Support TCP/UDP forwarding
- Implement firewall rules
- Implement NAT
- Add IPv6 support
- Measure packet-processing performance
- Explore multithreaded packet processing
- Explore DPDK
- Explore FD.io VPP

## Architecture

The project will progressively evolve from a basic
Linux packet-processing program into a functional
software router.

## Technology

- C
- Linux networking
- TCP/IP
- IPv4 / IPv6
- Ethernet
- ARP
- ICMP
- TCP / UDP
- Linux network namespaces
- Wireshark
- tcpdump
- DPDK
- FD.io VPP

## Roadmap

The router is built incrementally. Each milestone adds one capability and is documented with tests and packet captures.

**Progress: 2 / 18 milestones complete**

### Phase 1: Foundations

| #  | Milestone                   | What it covers                                        | Status      | Docs |
|----|-----------------------------|-------------------------------------------------------|-------------|------|
| M0 | Project foundation          | Repository layout, Makefile, Clang toolchain          | Done        | |
| M1 | Network namespace lab       | `pc-a` ↔ `router` ↔ `pc-b` with veth pairs           | Done        | [M1](docs/m1-lab.md) |
| M2 | Packet capture              | Raw sockets, receiving Ethernet frames in C           | In progress | |
| M3 | Ethernet + IPv4 parsing     | Header parsing, addresses, TTL, checksums             | Planned     | |

### Phase 2: A working router

| #  | Milestone                   | What it covers                                        | Status      | Docs |
|----|-----------------------------|-------------------------------------------------------|-------------|------|
| M4 | Routing table               | Longest-prefix match lookup                           | Planned     | |
| M5 | Packet forwarding           | TTL decrement, checksum update, transmit              | Planned     | |
| M6 | ICMP                        | Echo reply, TTL exceeded, destination unreachable     | Planned     | |
| M7 | ARP                         | IP-to-MAC resolution, neighbor table                  | Planned     | |
| M8 | TCP/UDP traffic             | HTTP, SSH and DNS through the router                  | Planned     | |

### Phase 3: Security and operations

| #   | Milestone                  | What it covers                                        | Status      | Docs |
|-----|----------------------------|-------------------------------------------------------|-------------|------|
| M9  | Firewall                   | Rule-based packet filtering                           | Planned     | |
| M10 | NAT                        | SNAT, PAT, connection tracking                        | Planned     | |
| M11 | Configuration              | Config file instead of hardcoded values               | Planned     | |
| M12 | Observability              | Counters, drops, logs                                 | Planned     | |

### Phase 4: Performance and beyond

| #   | Milestone                  | What it covers                                        | Status      | Docs |
|-----|----------------------------|-------------------------------------------------------|-------------|------|
| M13 | Benchmarking               | Throughput, latency, CPU with iperf3 and perf         | Planned     | |
| M14 | Multithreading             | Worker threads, queues, lock contention               | Planned     | |
| M15 | IPv6                       | IPv6 forwarding, ICMPv6, Neighbor Discovery           | Planned     | |
| M16 | DPDK                       | User-space packet I/O, poll-mode drivers              | Planned     | |
| M17 | FD.io VPP                  | Vector packet processing, comparison with this router | Planned     | |
