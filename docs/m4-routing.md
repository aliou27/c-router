# M4: Routing Table

## Goal

For every IPv4 packet, decide which interface it must leave through, using a routing table. This is the core decision of a router. At this stage the program prints the decision only; the Linux kernel still forwards the packets (sending them out is M5).

## Static routes

The router starts with an empty table, so the routes are written in the code before it runs. This is static routing, the same as typing `ip route` on a Cisco router:

| Cisco IOS | This router (`src/main.c`) |
|---|---|
| `ip route 10.0.1.0 255.255.255.0 Gi0/0` | `{ {10,0,1,0}, {255,255,255,0}, "eth0", "10.0.1.0/24" }` |
| `ip route 10.0.2.0 255.255.255.0 Gi0/1` | `{ {10,0,2,0}, {255,255,255,0}, "eth1", "10.0.2.0/24" }` |

These are the two networks directly connected to the router in the lab:

```text
pc-a ── 10.0.1.0/24 ── eth0 [ROUTER] eth1 ── 10.0.2.0/24 ── pc-b
```

Later milestones will load routes from a configuration file (M11) instead of the code.

## Matching a destination against a route

The packet carries one destination address (bytes 30-33 of the frame, decoded in M3). A route describes a range of addresses: a network and a mask.

For each of the 4 numbers of the address, the destination is ANDed with the mask and compared with the network, like the ANDing used in subnetting:

- `number & 255` keeps the number (this position must match)
- `number & 0` gives 0 (this position is ignored)

Example: destination `10.0.2.10` against the route `10.0.2.0 / 255.255.255.0`:

```text
10 & 255 = 10   vs 10   same
 0 & 255 = 0    vs 0    same
 2 & 255 = 2    vs 2    same
10 & 0   = 0    vs 0    same     -> match, send out eth1
```

Against `10.0.1.0 / 255.255.255.0`, the third number gives `2` instead of `1`, so it does not match.

## Code structure

```c
struct route {
    unsigned char network[4];
    unsigned char mask[4];
    const char   *door;   /* interface, e.g. "eth1" */
    const char   *text;   /* for printing, e.g. "10.0.2.0/24" */
};

#define ROUTE_COUNT 2
static const struct route routing_table[ROUTE_COUNT] = { ... };
```

- `route_matches(dest, line)`: compares the destination with one line of the table. Returns 1 if it fits, 0 otherwise.
- `lookup_route(dest)`: tries each line in order. Returns the number of the first line that fits, or -1 if none does.
- In `print_ipv4`, the destination IP is looked up and the result is printed: `route: <network> via <interface>`, or `route: none, drop`.

`ROUTE_COUNT` is used both for the size of the table and for the loop, so adding a route only requires changing one number.

## Algorithm

```text
FOR each line of the table:
    FOR each of the 4 numbers:
        IF (destination AND mask) != network: this line does not fit
    IF all 4 numbers fit: RETURN this line
RETURN "no route"
```

## Test 1: ping from pc-a to pc-b, captured on eth1

```bash
sudo ip netns exec router ./c-router eth1
sudo ip netns exec pc-a ping -c 3 10.0.2.10
```

![Route lookup on eth1](images/m4-route-lookup.png)

```text
Listening on eth1... (Ctrl+C to stop)
#1  42 bytes  9a:49:47:8b:c6:a6 -> ff:ff:ff:ff:ff:ff  ARP (0x0806)
#2  42 bytes  c2:d6:69:e3:55:eb -> 9a:49:47:8b:c6:a6  ARP (0x0806)
#3  98 bytes  9a:49:47:8b:c6:a6 -> c2:d6:69:e3:55:eb  IPv4 (0x0800)
    10.0.1.10 -> 10.0.2.10  TTL 63  ICMP  len 84
    route: 10.0.2.0/24 via eth1
#4  98 bytes  c2:d6:69:e3:55:eb -> 9a:49:47:8b:c6:a6  IPv4 (0x0800)
    10.0.2.10 -> 10.0.1.10  TTL 64  ICMP  len 84
    route: 10.0.1.0/24 via eth0
#5  98 bytes  9a:49:47:8b:c6:a6 -> c2:d6:69:e3:55:eb  IPv4 (0x0800)
    10.0.1.10 -> 10.0.2.10  TTL 63  ICMP  len 84
    route: 10.0.2.0/24 via eth1
#6  98 bytes  c2:d6:69:e3:55:eb -> 9a:49:47:8b:c6:a6  IPv4 (0x0800)
    10.0.2.10 -> 10.0.1.10  TTL 64  ICMP  len 84
    route: 10.0.1.0/24 via eth0
#7  98 bytes  9a:49:47:8b:c6:a6 -> c2:d6:69:e3:55:eb  IPv4 (0x0800)
    10.0.1.10 -> 10.0.2.10  TTL 63  ICMP  len 84
    route: 10.0.2.0/24 via eth1
#8  98 bytes  c2:d6:69:e3:55:eb -> 9a:49:47:8b:c6:a6  IPv4 (0x0800)
    10.0.2.10 -> 10.0.1.10  TTL 64  ICMP  len 84
    route: 10.0.1.0/24 via eth0
#9  42 bytes  c2:d6:69:e3:55:eb -> 9a:49:47:8b:c6:a6  ARP (0x0806)
#10  42 bytes  9a:49:47:8b:c6:a6 -> c2:d6:69:e3:55:eb  ARP (0x0806)
^C
--- Capture statistics (eth1) ---
Total frames : 10
  IPv4       : 6
  ARP        : 4
  IPv6       : 0
  Other      : 0
  Too short  : 0
Total bytes  : 756
```

**Result:** the requests to pc-b (10.0.2.10) match `10.0.2.0/24` and go out eth1. The replies to pc-a (10.0.1.10) match `10.0.1.0/24` and go out eth0. This is the same decision the Linux kernel made, since the ping succeeded.

## Test 2: destination with no route, captured on eth0

```bash
sudo ip netns exec router ./c-router eth0
sudo ip netns exec pc-a ping -c 2 8.8.8.8
```

![No route to 8.8.8.8](images/m4-no-route.png)

```text
Listening on eth0... (Ctrl+C to stop)
#1  98 bytes  12:bf:ee:cb:e0:59 -> ea:51:9b:72:98:9a  IPv4 (0x0800)
    10.0.1.10 -> 8.8.8.8  TTL 64  ICMP  len 84
    route: none, drop
#2  126 bytes  ea:51:9b:72:98:9a -> 12:bf:ee:cb:e0:59  IPv4 (0x0800)
    10.0.1.1 -> 10.0.1.10  TTL 64  ICMP  len 112
    route: 10.0.1.0/24 via eth0
#3  98 bytes  12:bf:ee:cb:e0:59 -> ea:51:9b:72:98:9a  IPv4 (0x0800)
    10.0.1.10 -> 8.8.8.8  TTL 64  ICMP  len 84
    route: none, drop
#4  126 bytes  ea:51:9b:72:98:9a -> 12:bf:ee:cb:e0:59  IPv4 (0x0800)
    10.0.1.1 -> 10.0.1.10  TTL 64  ICMP  len 112
    route: 10.0.1.0/24 via eth0
#5  42 bytes  ea:51:9b:72:98:9a -> 12:bf:ee:cb:e0:59  ARP (0x0806)
#6  42 bytes  12:bf:ee:cb:e0:59 -> ea:51:9b:72:98:9a  ARP (0x0806)
#7  42 bytes  ea:51:9b:72:98:9a -> 12:bf:ee:cb:e0:59  ARP (0x0806)
#8  42 bytes  12:bf:ee:cb:e0:59 -> ea:51:9b:72:98:9a  ARP (0x0806)
^C
--- Capture statistics (eth0) ---
Total frames : 8
  IPv4       : 4
  ARP        : 4
  IPv6       : 0
  Other      : 0
  Too short  : 0
Total bytes  : 616
```

**Result:** `8.8.8.8` matches no line of the table, so the program reports `route: none, drop`. There is no default route (`0.0.0.0/0`) in the table.

### The 126-byte packets from 10.0.1.1

Frames #2 and #4 were not sent by pc-a. They come from the router itself (10.0.1.1 is router eth0). The Linux kernel on the router also has no route to 8.8.8.8, so it answers pc-a with an **ICMP Destination Unreachable** message.

Its size shows what it contains:

```text
20 bytes   new IPv4 header      (10.0.1.1 -> 10.0.1.10)
 8 bytes   ICMP "unreachable" header
84 bytes   copy of pc-a's original packet, so pc-a knows which packet failed
---------
112 bytes  IPv4 length  (+ 14 Ethernet = 126 bytes on the wire)
```

This is the behaviour the C router will implement itself in M6 (ICMP).

### Byte count check

```text
2 x 98  (pings to 8.8.8.8)    = 196
2 x 126 (ICMP unreachable)    = 252
4 x 42  (ARP)                 = 168
                        Total = 616 bytes
```

## Limitations

- **No default route.** Any destination outside the two lab networks has no route.
- **First match, not longest match.** The lookup returns the first line that fits. With the two current routes only one can fit, so this is correct. When routes overlap (for example `10.0.0.0/16` and `10.0.2.0/24`, or a default route `0.0.0.0/0`), the most specific route must win: this is longest prefix match, the next step of M4.
- **Decision only.** The packet is not forwarded by the program yet (M5).

## Status

- [x] Static routing table in the code
- [x] Route lookup with network and mask
- [x] Tested: routed traffic and a destination with no route
- [ ] Default route and longest prefix match
