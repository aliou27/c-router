# M2: Packet Capture

## Goal

Write a C program that receives raw Ethernet frames from a network interface of the router namespace and displays them. This is the first building block of the router: every later feature (parsing, routing, firewall, NAT) starts with receiving a packet.

At this stage the program is a pure observer. The Linux kernel still forwards packets (`net.ipv4.ip_forward=1`). The program receives a copy of each frame, like tcpdump.

## Key concepts

- **Kernel space / user space:** a normal program cannot access the network card directly. It asks the Linux kernel through system calls.
- **Socket:** a communication channel managed by the kernel.
- **Raw socket (`AF_PACKET`, `SOCK_RAW`):** delivers complete Ethernet frames, headers included, for all traffic on the interface. It requires root privileges because it can see every packet.
- **File descriptor:** the small integer returned by `socket()` that identifies the socket in all later calls.
- **Interface index:** the kernel identifies interfaces by number, not by name. `if_nametoindex("eth1")` converts the name to its number.
- **bind:** attaches the socket to a single interface, so it only receives frames from that interface.
- **recv (blocking):** waits until a frame arrives, then copies it into a buffer and returns its size.
- **Buffer:** a 2048-byte array that receives each frame. The largest standard Ethernet frame is 1514 bytes.
- **Byte order:** network protocols use big-endian order. `htons()` converts values from host order to network order.

No external library is used, only the C standard library and Linux system headers. libpcap was deliberately avoided in order to work directly with the kernel interface, and because the same raw socket will later be used to transmit packets (M5).

## Algorithm

```text
0. Check that an interface name was given
1. Open a raw socket for all protocols         (socket)
2. Convert the interface name to its index     (if_nametoindex)
3. Attach the socket to that interface         (bind)
4. Loop forever:
     wait for a frame and copy it into the buffer   (recv)
     display its size and its first 16 bytes
5. Close the socket                            (close)
```

Every system call is checked for errors, and the reason is displayed with `perror`.

## How to run

```bash
make
make lab-up
sudo ip netns exec router ./c-router eth1
```

In a second terminal:

```bash
sudo ip netns exec pc-a ping -c 3 10.0.2.10
```

## Error cases tested

| Command | Result | Reason |
|---|---|---|
| `./c-router` | Usage message | No interface given |
| `./c-router eth1` | `socket: Operation not permitted` | Raw sockets require root |
| `sudo ip netns exec router ./c-router eth9` | `if_nametoindex: No such device` | Interface does not exist |

## Capture 1: with IPv6 enabled

The program was started right after `make lab-up`, before any ping:
![alt text](image.png)

Frames were captured even without a ping. Bytes 12-13 (`86 dd`) show they are IPv6. Linux enables IPv6 automatically on every interface, and each interface announces itself when it comes up:

- Destination `33:33:...` is an IPv6 multicast MAC address.
- 86-byte frames to `33:33:ff:...`: Duplicate Address Detection ("is anyone already using my IPv6 address?").
- 90-byte frames to `33:33:00:00:00:16`: multicast group membership reports.
- 70-byte frames to `33:33:00:00:00:02`: Router Solicitations ("is there an IPv6 router here?"), repeated because nobody answers.

To keep captures focused on IPv4 until M15, IPv6 is now disabled in the lab script (`net.ipv6.conf.all.disable_ipv6=1` in each namespace).

![alt text](image-1.png)



## Capture 2: IPv6 disabled, 3 pings from pc-a to pc-b

```text
#1  42 bytes  ff ff ff ff ff ff 0a a0 3b 81 74 77 08 06 00 01
#2  42 bytes  0a a0 3b 81 74 77 02 58 29 f9 1a 9c 08 06 00 01
#3  98 bytes  02 58 29 f9 1a 9c 0a a0 3b 81 74 77 08 00 45 00
#4  98 bytes  0a a0 3b 81 74 77 02 58 29 f9 1a 9c 08 00 45 00
#5  98 bytes  02 58 29 f9 1a 9c 0a a0 3b 81 74 77 08 00 45 00
#6  98 bytes  0a a0 3b 81 74 77 02 58 29 f9 1a 9c 08 00 45 00
#7  98 bytes  02 58 29 f9 1a 9c 0a a0 3b 81 74 77 08 00 45 00
#8  98 bytes  0a a0 3b 81 74 77 02 58 29 f9 1a 9c 08 00 45 00
#9  42 bytes  0a a0 3b 81 74 77 02 58 29 f9 1a 9c 08 06 00 01
#10  42 bytes  02 58 29 f9 1a 9c 0a a0 3b 81 74 77 08 06 00 01
```

![alt text](image-2.png)

### Identifying the machines

Frame #3 is the ping leaving the router toward pc-b, so:

- `0a:a0:3b:81:74:77` = router eth1
- `02:58:29:f9:1a:9c` = pc-b

### Reading the frames

Each line shows: destination MAC (bytes 0-5), source MAC (bytes 6-11), type (bytes 12-13), then the start of the payload.

| Frames | Type | Meaning |
|---|---|---|
| #1 | `08 06` ARP | The router broadcasts to `ff:ff:ff:ff:ff:ff`: "Who has 10.0.2.10?" |
| #2 | `08 06` ARP | pc-b answers the router directly with its MAC address |
| #3, #5, #7 | `08 00` IPv4 | ICMP echo request, router to pc-b |
| #4, #6, #8 | `08 00` IPv4 | ICMP echo reply, pc-b to router |
| #9 | `08 06` ARP | pc-b checks that the router's MAC is still valid (unicast) |
| #10 | `08 06` ARP | The router confirms |

### Observations

- **ARP comes first.** The router cannot send the ping until it knows pc-b's MAC address. This is the mechanism that will be implemented in M7.
- **Frame sizes match the protocol layers.** IPv4 frames: 14 (Ethernet) + 20 (IPv4) + 64 (ICMP) = 98 bytes. ARP frames: always 42 bytes.
- **`45` at byte 14** is the first byte of the IPv4 header: version 4, header length 5 x 4 = 20 bytes.
- **MAC addresses change between runs.** Linux assigns a random MAC address to each veth interface when it is created. The router code must never assume fixed MAC addresses.

## Status

- [x] Step 1: raw socket capture on one interface
- [ ] Step 2: decode the Ethernet header (MAC addresses and type)
- [ ] Step 3: packet counters and statistics on exit

![alt text](image.png)## Step 2: Decoding the Ethernet header

### Goal

Replace the raw byte dump with a readable line showing the source MAC, the destination MAC and the protocol carried by the frame.

### Ethernet header layout

The Ethernet header is always the first 14 bytes of the frame:

```text
byte:   0  1  2  3  4  5 | 6  7  8  9 10 11 | 12 13
        destination MAC  | source MAC       | EtherType
```

### Logic

- **MAC addresses:** bytes 0-5 (destination) and 6-11 (source) are printed as six 2-digit hex values separated by `:`. The source is printed first so the output reads as `sender -> receiver`.
- **EtherType:** bytes 12-13 form one 16-bit number in network byte order (big-endian): `type = byte12 x 256 + byte13`, written in C as `(frame[12] << 8) | frame[13]`.

| EtherType | Protocol |
|-----------|----------|
| `0x0800`  | IPv4     |
| `0x0806`  | ARP      |
| `0x86DD`  | IPv6     |

- **Length check:** a frame shorter than 14 bytes is reported and skipped. Without this check, the program would read bytes left over from the previous frame in the buffer (an out-of-bounds read). This is the first input validation in the router.
- **Pointers instead of copies:** the MAC addresses are not copied. The program uses pointers to their position in the buffer (`frame` and `frame + 6`).

### Code structure

Two helper functions keep the receive loop short:

- `print_mac(const unsigned char *mac)`: prints 6 bytes as `aa:bb:cc:dd:ee:ff`.
- `ethertype_name(unsigned int type)`: returns `"IPv4"`, `"ARP"`, `"IPv6"` or `"other"`.

### Algorithm

```text
FOR each received frame:
    IF size < 14 THEN
        display "too short", skip the frame
    destination <- bytes 0-5
    source      <- bytes 6-11
    type        <- byte12 x 256 + byte13
    display: count, size, source -> destination, protocol name
```

### Capture: 3 pings from pc-a to pc-b

```text
PASTE YOUR OUTPUT HERE
```

![Decoded Ethernet capture](images/m2-step2-decoded.png)

### Observations

- The same frames as Step 1 are now readable without counting bytes by hand.
- ARP requests go to `ff:ff:ff:ff:ff:ff` (broadcast). ARP replies and pings are sent directly between the two MAC addresses (unicast).
- The EtherType is enough to know how to read the rest of the frame. Step M3 will use it to decide whether to parse an IPv4 header.

## Status

- [x] Step 1: raw socket capture on one interface
- [x] Step 2: decode the Ethernet header (MAC addresses and type)
- [ ] Step 3: packet counters and statistics on exit
