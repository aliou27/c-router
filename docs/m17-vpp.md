# M17: FD.io VPP

## Goal

Replace the C router with **FD.io VPP** (Vector Packet Processing), the open source user-space router used in production, on the same lab. Then compare what VPP does with what this project's router does, step by step.

## Setup

VPP has no packages for Ubuntu 26.04 (the `router` VM). FD.io publishes arm64 builds up to Ubuntu 24.04, so VPP runs in a second Lima VM:

```bash
limactl start --name=vpp --cpus=2 --memory=4 --mount-writable template://ubuntu-24.04
limactl shell vpp
curl -s https://packagecloud.io/install/repositories/fdio/release/script.deb.sh | sudo bash
sudo apt install -y vpp vpp-plugin-core make ethtool traceroute
```

```text
vpp v26.06-release built by root on 43a851bd4e66 at 2026-06-24T09:27:24
```

At startup systemd reports `modprobe uio_pci_generic ... FAILURE`. That module binds a physical NIC to DPDK and is not needed here.

## Topology

```text
   pc-a  10.0.1.10                                    pc-b  10.0.2.10
     |                                                    |
   eth0 ---- vpp-a   [ VPP ]   vpp-b ------------------ eth0
             host-vpp-a        host-vpp-b
             10.0.1.1          10.0.2.1
```

VPP runs as a service in the VM's main namespace, so the router end of each veth (`vpp-a`, `vpp-b`) stays there. The PCs keep the same MACs, IPs and gateways as the C router lab.

```bash
make vpp-up
make vpp-down
```

`scripts/vpp-up.sh` builds the namespaces and cables, then configures VPP:

```bash
vppctl create host-interface name vpp-a hw-addr 02:00:00:00:01:01
vppctl create host-interface name vpp-b hw-addr 02:00:00:00:02:01
vppctl set interface ip address host-vpp-a 10.0.1.1/24
vppctl set interface ip address host-vpp-b 10.0.2.1/24
vppctl set interface state host-vpp-a up
vppctl set interface state host-vpp-b up
```

`host-interface` attaches VPP to a Linux interface with an **AF_PACKET** socket, the same mechanism the C router uses.

Note: the veth is created as `ip link add vpp-a type veth peer name eth0 netns pc-a`. Writing it the other way (`ip link add eth0 netns pc-a ...`) fails with `File exists`, because the VM already has its own `eth0`.

## C router vs VPP

| Job | C router | VPP |
|---|---|---|
| Attach to an interface | `open_raw_socket()` (AF_PACKET) | `create host-interface` (AF_PACKET) |
| Interface IP and MAC | `ifaces[]` in the code | `set interface ip address`, `hw-addr` |
| Routing table | `routing_table[]` typed by hand | connected routes added automatically |
| Neighbor MACs | static `neighbors[]` (no ARP) | ARP, learned automatically |
| Packets per step | 1 | a vector (batch) of up to 256 |
| ICMP errors | always sent | rate limited |
| Checksum after TTL | full recompute | incremental update |

## Test 1: interfaces and FIB

```text
$ sudo vppctl show interface addr
host-vpp-a (up):
  L3 10.0.1.1/24
host-vpp-b (up):
  L3 10.0.2.1/24
local0 (dn):

$ sudo vppctl show ip fib 10.0.2.10
10.0.2.0/24 fib:0 index:12 locks:2
  interface refs:1 entry-flags:connected,attached, ...
      path:[23] ... attached:  oper-flags:resolved, cfg-flags:glean,
         host-vpp-b
  [@0]: dpo-load-balance: ...
    [0] [@4]: ipv4-glean: [src:10.0.2.0/24] host-vpp-b: ... ffffffffffff0200000002010806
```

- Giving `host-vpp-b` the address 10.0.2.1/24 created the **connected** route 10.0.2.0/24. Nobody typed it, unlike `routing_table[]` in the C router.
- **glean** means "the network is attached, but the MAC of this host is not known yet: send an ARP request". The prepared header ends in `ffffffffffff` (broadcast) and `0806` (ARP EtherType).
- `local0` is a placeholder interface VPP always creates. It never carries traffic.

## Test 2: forwarding

```text
$ sudo ip netns exec pc-a ping -c 3 10.0.2.10
64 bytes from 10.0.2.10: icmp_seq=2 ttl=63 time=3.20 ms
64 bytes from 10.0.2.10: icmp_seq=3 ttl=63 time=3.16 ms
3 packets transmitted, 2 received, 33.3333% packet loss

$ sudo vppctl show ip neighbors
      Age                       IP                    Flags      Ethernet              Interface
      3.0670                10.0.1.10                  D    02:00:00:00:01:0a host-vpp-a
      3.0639                10.0.2.10                  D    02:00:00:00:02:0a host-vpp-b
```

- `ttl=63`: one router hop, as with the C router.
- **The first ping is lost.** VPP had no MAC for 10.0.2.10, so the first packet hit the glean path: VPP sent an ARP request and dropped that packet instead of queueing it. Once the reply arrived, the next pings passed. Linux would queue the packet, VPP drops it to keep the fast path simple.
- Flag `D` = **dynamic**: both entries were learned by ARP. The C router needed them typed by hand in `neighbors[]`.

## Test 3: traceroute

```text
$ sudo ip netns exec pc-a traceroute -I -n 10.0.2.10
 1  10.0.1.1  1.191 ms * *
 2  10.0.2.10  2.874 ms  2.884 ms  2.885 ms
```

Hop 1 is VPP (10.0.1.1), as in M6. Two of the three probes show `*`: VPP **rate limits ICMP errors**, so it answered only the first Time Exceeded. This is exactly the limitation listed in M6 for the C router.

## Test 4: packet trace through the node graph

```bash
sudo vppctl clear trace
sudo vppctl trace add af-packet-input 5
sudo ip netns exec pc-a ping -c 1 10.0.2.10
sudo vppctl show trace
```

Packet 1 (echo request, pc-a to pc-b), shortened:

```text
af-packet-input     af_packet: hw_if_index 1 rx-queue 0
ethernet-input      IP4: 02:00:00:00:01:0a -> 02:00:00:00:01:01
ip4-input           ICMP: 10.0.1.10 -> 10.0.2.10  ttl 64, length 84, checksum 0x82a5
ip4-lookup          fib 0 dpo-idx 5
ip4-rewrite         ipv4 via 10.0.2.10 host-vpp-b: 02000000020a0200000002010800
host-vpp-b-output   IP4: 02:00:00:00:02:01 -> 02:00:00:00:02:0a
                    ICMP: 10.0.1.10 -> 10.0.2.10  ttl 63, checksum 0x83a5
host-vpp-b-tx       af_packet: hw_if_index 2 tx-queue 0
```

Packet 2 (echo reply) takes the same path in the other direction, from `host-vpp-b` to `host-vpp-a`.

### Each node next to the C router

| VPP node | C router (`handle_packet`) |
|---|---|
| `af-packet-input` | `recvfrom()` on the raw socket |
| `ethernet-input` | check EtherType 0x0800 and destination MAC |
| `ip4-input` | validate version, header length, checksum |
| `ip4-lookup` | `lookup_route()`, longest prefix match |
| `ip4-rewrite` | TTL - 1, checksum, new MAC header |
| `host-vpp-b-output` / `-tx` | `send()` on the output socket |

The same steps, in the same order. The difference is how they run: the C router takes **one** packet through all steps, then the next. VPP takes a **vector** of packets through `ip4-input`, then the whole vector through `ip4-lookup`, and so on. Each node's code and data stay in the CPU cache for the whole batch, which is where VPP's speed comes from.

### The rewrite header

```text
02000000020a  0200000002 01  0800
dst MAC       src MAC         EtherType IPv4
pc-b          VPP host-vpp-b
```

VPP builds this 14-byte header **once**, when the neighbor is learned, and stores it in the adjacency. For each packet it only copies it. The C router writes the MACs field by field for every packet.

### Incremental checksum

| | TTL | Checksum |
|---|---|---|
| Before | 64 (0x40) | 0x82a5 |
| After | 63 (0x3f) | 0x83a5 |

TTL sits in the high byte of a 16-bit word. Subtracting 1 from it lowers the sum by 0x0100, so the checksum (65535 minus the sum) goes **up** by 0x0100: 0x82a5 + 0x0100 = 0x83a5. VPP applies this small update directly (RFC 1624) instead of adding up the whole header again like `ipv4_checksum()` does. Same result, less work.

## What this shows

- The C router implements the same pipeline as a production router: input, validation, lookup, rewrite, output.
- VPP adds what a real product needs: ARP, connected routes, ICMP rate limiting, incremental checksum, precomputed rewrite headers, and batch (vector) processing.
- Both attach to Linux with AF_PACKET here. In production VPP uses DPDK drivers instead, to read the NIC directly without the kernel (M16).

## Status

- [x] VPP 26.06 installed on arm64 (Ubuntu 24.04 VM)
- [x] Lab script: `make vpp-up` / `make vpp-down`
- [x] Ping pc-a to pc-b through VPP (ttl=63)
- [x] ARP learned automatically, connected routes, glean
- [x] traceroute: VPP as hop 1, ICMP rate limiting visible
- [x] Packet trace mapped to the C router's steps

**M17 complete.**
