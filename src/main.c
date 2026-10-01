#define _DEFAULT_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <poll.h>                /* M5: poll, watch several sockets at once */
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define ETH_HEADER_LEN 14

/* ---- The counting paper ---- */
struct stats {
    unsigned long      total;
    unsigned long      ipv4;
    unsigned long      arp;
    unsigned long      ipv6;
    unsigned long      other;
    unsigned long      too_short;
    unsigned long long bytes;
};

/* ---- The Ctrl+C light: 0 = keep going, 1 = stop ---- */
static volatile sig_atomic_t stop_requested = 0;

/* ---- Called by Linux when Ctrl+C is pressed: turns the light on ---- */
static void on_sigint(int signum)
{
    (void)signum;
    stop_requested = 1;
}

/* ---- Print 6 boxes as a MAC address: 02:58:29:f9:1a:9c ---- */
static void print_mac(const unsigned char *mac)
{
    for (int i = 0; i < 6; i++) {
        printf("%02x", mac[i]);
        if (i < 5) {
            printf(":");
        }
    }
}

/* ---- Boxes 12-13 number -> word ---- */
static const char *ethertype_name(unsigned int type)
{
    if (type == 0x0800) return "IPv4";
    if (type == 0x0806) return "ARP";
    if (type == 0x86DD) return "IPv6";
    return "other";
}

/* ---- M3: print 4 boxes as an IP address: 10.0.1.10 ---- */
static void print_ip(const unsigned char *ip)
{
    printf("%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

/* ---- M3: box 23 number -> word ---- */
static const char *protocol_name(unsigned int protocol)
{
    if (protocol == 1)  return "ICMP";
    if (protocol == 6)  return "TCP";
    if (protocol == 17) return "UDP";
    return "other";
}

/* ---- M4: one line of the routing table ---- */
struct route {
    unsigned char network[4];   /* e.g. 10.0.2.0      */
    unsigned char mask[4];      /* e.g. 255.255.255.0 */
    int           prefix;       /* e.g. 24 (the /24): how precise the route is */
    const char   *door;         /* e.g. "eth1"        */
    const char   *text;         /* e.g. "10.0.2.0/24" (for printing) */
};

/* ---- M4: how many lines the table has. Change it when adding a route. ---- */
#define ROUTE_COUNT 5

/* ---- M4: the routing table, our "ip route" commands ---- */
/* The default route is first on purpose: the order must not matter. */
static const struct route routing_table[ROUTE_COUNT] = {
    { {0, 0, 0, 0},    {0, 0, 0, 0},         0,  "eth0", "0.0.0.0/0"    },   /* line 0: default route */
    { {10, 0, 0, 0},   {255, 255, 0, 0},     16, "eth0", "10.0.0.0/16"  },   /* line 1 */
    { {10, 0, 1, 0},   {255, 255, 255, 0},   24, "eth0", "10.0.1.0/24"  },   /* line 2 */
    { {10, 0, 2, 0},   {255, 255, 255, 0},   24, "eth1", "10.0.2.0/24"  },   /* line 3 */
    { {10, 0, 2, 10},  {255, 255, 255, 255}, 32, "eth1", "10.0.2.10/32" },   /* line 4: host route */
};

/* ---- M4: does the destination fit in this line? 1 = yes, 0 = no ---- */
static int route_matches(const unsigned char *dest, int line)
{
    for (int i = 0; i < 4; i++) {
        if ((dest[i] & routing_table[line].mask[i]) != routing_table[line].network[i]) {
            return 0;   /* different: does not fit */
        }
    }
    return 1;           /* all 4 numbers match: fits */
}

/* ---- M4: check every line, keep the most precise one that fits (biggest /) ---- */
/* Returns the winning line number, or -1 if no line fits. */
static int lookup_route(const unsigned char *dest)
{
    int best = -1;                          /* no winner yet */

    for (int line = 0; line < ROUTE_COUNT; line++) {
        if (route_matches(dest, line)) {
            if (best == -1 || routing_table[line].prefix > routing_table[best].prefix) {
                best = line;                /* more precise than the previous winner */
            }
        }
    }
    return best;
}

/* ---- M5: IPv4 header checksum ----
 * header      = where the IPv4 header starts (frame + 14)
 * header_size = its length in bytes (20 without options)
 * 1. glue the header into 2-byte numbers and add them,
 *    skipping bytes 10-11 (the checksum itself counts as 0)
 * 2. fold: while the sum is too big for 2 bytes, add the overflow back in
 * 3. flip: 65535 - sum
 */
static unsigned int ipv4_checksum(const unsigned char *header, unsigned int header_size)
{
    unsigned long sum = 0;

    for (unsigned int i = 0; i < header_size; i += 2) {
        if (i == 10) {
            continue;                       /* skip the checksum field */
        }
        sum += header[i] * 256 + header[i + 1];
    }

    while (sum > 65535) {
        sum = (sum % 65536) + (sum / 65536);
    }

    return 65535 - (unsigned int)sum;
}

/* ---- M3: read and print the IPv4 part (boxes 14 to 33) ---- */
static void print_ipv4(const unsigned char *frame, size_t size)
{
    /* 1. Long enough? 14 (Ethernet) + 20 (IPv4) = 34 boxes */
    if (size < 34) {
        printf("    IPv4 too short (%zu bytes)\n", size);
        return;
    }

    /* 2. Split box 14 into two hex digits: 45 -> 4 and 5 */
    unsigned int version     = frame[14] / 16;          /* 4 */
    unsigned int header_size = (frame[14] % 16) * 4;    /* 5 x 4 = 20 */

    /* 3. Check version and header size */
    if (version != 4) {
        printf("    not IPv4 (version %u)\n", version);
        return;
    }
    if (header_size < 20) {
        printf("    bad header size (%u)\n", header_size);
        return;
    }
    if (14 + header_size > size) {
        printf("    header longer than the frame\n");
        return;
    }

    /* 4. Total size: glue boxes 16 and 17 */
    unsigned int total_size = frame[16] * 256 + frame[17];   /* 0 x 256 + 84 = 84 */

    /* 5. Is the total size honest? */
    if (total_size < header_size || 14 + total_size > size) {
        printf("    lying total size (%u)\n", total_size);
        return;
    }

    /* 6. Read the rest */
    unsigned int ttl      = frame[22];
    unsigned int protocol = frame[23];
    const unsigned char *from_ip = frame + 26;   /* finger on box 26 */
    const unsigned char *to_ip   = frame + 30;   /* finger on box 30 */

    /* 7. Print one line */
    printf("    ");
    print_ip(from_ip);
    printf(" -> ");
    print_ip(to_ip);
    printf("  TTL %u  %s  len %u\n", ttl, protocol_name(protocol), total_size);

    /* 8. M4: which door? */
    int line = lookup_route(to_ip);
    if (line == -1) {
        printf("    route: none, drop\n");
    } else {
        printf("    route: %s via %s\n", routing_table[line].text, routing_table[line].door);
    }

    /* 9. M5: is the checksum in the packet correct? */
    const unsigned char *header = frame + 14;              /* the IPv4 header starts at byte 14 */
    unsigned int stored   = frame[24] * 256 + frame[25];   /* checksum = bytes 24-25 */
    unsigned int computed = ipv4_checksum(header, header_size);
    printf("    checksum: 0x%04x %s\n", stored, computed == stored ? "OK" : "BAD");

    /* 10. M5: what forwarding would change (on a copy, the packet is not modified) */
    if (ttl <= 1) {
        printf("    forward: TTL %u, expired, drop\n", ttl);
    } else {
        unsigned char copy[60];                            /* an IPv4 header is at most 60 bytes */
        memcpy(copy, header, header_size);
        copy[8] = (unsigned char)(ttl - 1);                /* TTL = byte 8 of the header (byte 22 of the frame) */
        unsigned int new_checksum = ipv4_checksum(copy, header_size);
        printf("    forward: TTL %u -> %u, checksum 0x%04x -> 0x%04x\n",
               ttl, ttl - 1, stored, new_checksum);
    }
}

/* ---- Print the counting paper ---- */
static void print_stats(const struct stats *s, const char *interfaceName)
{
    printf("\n--- Capture statistics (%s) ---\n", interfaceName);
    printf("Total frames : %lu\n", s->total);
    printf("  IPv4       : %lu\n", s->ipv4);
    printf("  ARP        : %lu\n", s->arp);
    printf("  IPv6       : %lu\n", s->ipv6);
    printf("  Other      : %lu\n", s->other);
    printf("  Too short  : %lu\n", s->too_short);
    printf("Total bytes  : %llu\n", s->bytes);
}

/* ======================================================================
 * M5: FORWARDING
 * Run with:  sudo ip netns exec router ./c-router --forward
 * The program listens on eth0 AND eth1, and forwards IPv4 packets itself.
 * ====================================================================== */

/* ---- M5: the router's two doors (fixed MACs, set in scripts/netns-up.sh) ---- */
struct iface {
    const char   *name;     /* "eth0" */
    unsigned char mac[6];   /* the router's own MAC on this door */
    unsigned char ip[4];    /* the router's own IP on this door */
    int           sock;     /* the raw socket opened on this door */
};

#define IFACE_COUNT 2
static struct iface ifaces[IFACE_COUNT] = {
    { "eth0", {0x02, 0x00, 0x00, 0x00, 0x01, 0x01}, {10, 0, 1, 1}, -1 },
    { "eth1", {0x02, 0x00, 0x00, 0x00, 0x02, 0x01}, {10, 0, 2, 1}, -1 },
};

/* ---- M5: the neighbours' MACs (static ARP, like "arp 10.0.2.10 ..." on Cisco) ---- */
struct neighbor {
    unsigned char ip[4];
    unsigned char mac[6];
};

#define NEIGHBOR_COUNT 2
static const struct neighbor neighbors[NEIGHBOR_COUNT] = {
    { {10, 0, 1, 10}, {0x02, 0x00, 0x00, 0x00, 0x01, 0x0a} },   /* pc-a */
    { {10, 0, 2, 10}, {0x02, 0x00, 0x00, 0x00, 0x02, 0x0a} },   /* pc-b */
};

/* ---- M5: forwarding counters ---- */
static unsigned long forwarded = 0;
static unsigned long dropped   = 0;

/* ---- M5: open a raw socket bound to one door (same steps as M2). -1 on error ---- */
static int open_raw_socket(const char *name)
{
    int sock = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (sock < 0) {
        perror("socket");
        return -1;
    }

    unsigned int index = if_nametoindex(name);
    if (index == 0) {
        perror(name);
        close(sock);
        return -1;
    }

    struct sockaddr_ll form;
    memset(&form, 0, sizeof form);
    form.sll_family   = AF_PACKET;
    form.sll_protocol = htons(ETH_P_ALL);
    form.sll_ifindex  = (int)index;

    if (bind(sock, (struct sockaddr *)&form, sizeof form) < 0) {
        perror("bind");
        close(sock);
        return -1;
    }
    return sock;
}

/* ---- M5: which door has this name? Returns 0, 1, or -1 ---- */
static int find_iface(const char *name)
{
    for (int i = 0; i < IFACE_COUNT; i++) {
        if (strcmp(ifaces[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* ---- M5: the MAC of a neighbour, or NULL if unknown ---- */
static const unsigned char *find_neighbor_mac(const unsigned char *ip)
{
    for (int i = 0; i < NEIGHBOR_COUNT; i++) {
        if (memcmp(neighbors[i].ip, ip, 4) == 0) {
            return neighbors[i].mac;
        }
    }
    return NULL;
}

/* ---- M5: print why a packet was dropped ---- */
static void drop(const char *in_name, const unsigned char *frame, const char *reason)
{
    dropped++;
    printf("[%s] DROP  ", in_name);
    print_ip(frame + 26);
    printf(" -> ");
    print_ip(frame + 30);
    printf("  %s\n", reason);
}

/* ---- M6: checksum over any block of bytes (used for ICMP) ----
 * Same 3 steps as ipv4_checksum: add 2-byte pairs, fold, flip.
 * The checksum field inside the block must be 0 before calling it.
 */
static unsigned int inet_checksum(const unsigned char *data, unsigned int length)
{
    unsigned long sum = 0;

    for (unsigned int i = 0; i + 1 < length; i += 2) {
        sum += data[i] * 256 + data[i + 1];
    }
    if (length % 2 == 1) {
        sum += data[length - 1] * 256;          /* odd length: last byte alone */
    }

    while (sum > 65535) {
        sum = (sum % 65536) + (sum / 65536);
    }
    return 65535 - (unsigned int)sum;
}

/* ---- M6: ICMP message types ---- */
#define ICMP_DEST_UNREACHABLE  3     /* code 0 = network, code 1 = host */
#define ICMP_TIME_EXCEEDED     11    /* code 0 = TTL reached 0 in transit */

static unsigned long icmp_sent = 0;

/* ---- M6: send an ICMP error back to the sender of "orig" ----
 * in          = the door the original packet came in on (the error goes back out of it)
 * orig        = the original frame
 * header_size = the original IPv4 header size (20 without options)
 * total_size  = the original IPv4 total length
 */
static void send_icmp_error(int in, const unsigned char *orig,
                            unsigned int header_size, unsigned int total_size,
                            unsigned char type, unsigned char code)
{
    /* Rule: never answer an ICMP error with an ICMP error (only echo request/reply may trigger one) */
    if (orig[23] == 1) {
        if (total_size <= header_size) return;
        unsigned char orig_icmp_type = orig[14 + header_size];
        if (orig_icmp_type != 0 && orig_icmp_type != 8) return;
    }

    /* What we copy back: the original IP header + its first 8 data bytes (if present) */
    unsigned int copy_len = header_size + 8;
    if (copy_len > total_size) copy_len = total_size;

    unsigned char pkt[14 + 20 + 8 + 68];                 /* 68 = max header (60) + 8 */
    memset(pkt, 0, sizeof pkt);
    unsigned char *ip   = pkt + 14;
    unsigned char *icmp = ip + 20;
    unsigned int ip_total = 20 + 8 + copy_len;

    /* 1. Ethernet: back to the sender, from this door */
    memcpy(pkt,     orig + 6,        6);                 /* TO   = original source MAC */
    memcpy(pkt + 6, ifaces[in].mac,  6);                 /* FROM = this door's MAC */
    pkt[12] = 0x08;
    pkt[13] = 0x00;                                      /* type = IPv4 */

    /* 2. IPv4 header */
    ip[0]  = 0x45;                                       /* version 4, header 20 bytes */
    ip[2]  = (unsigned char)(ip_total / 256);
    ip[3]  = (unsigned char)(ip_total % 256);
    ip[8]  = 64;                                         /* TTL */
    ip[9]  = 1;                                          /* protocol = ICMP */
    memcpy(ip + 12, ifaces[in].ip, 4);                   /* FROM = this door's IP (e.g. 10.0.1.1) */
    memcpy(ip + 16, orig + 26,     4);                   /* TO   = original sender's IP */
    unsigned int ip_sum = ipv4_checksum(ip, 20);
    ip[10] = (unsigned char)(ip_sum / 256);
    ip[11] = (unsigned char)(ip_sum % 256);

    /* 3. ICMP: type, code, checksum (0 for now), 4 unused bytes, then the copy */
    icmp[0] = type;
    icmp[1] = code;
    memcpy(icmp + 8, orig + 14, copy_len);
    unsigned int icmp_sum = inet_checksum(icmp, 8 + copy_len);
    icmp[2] = (unsigned char)(icmp_sum / 256);
    icmp[3] = (unsigned char)(icmp_sum % 256);

    /* 4. Send it back out of the door it came in on */
    if (send(ifaces[in].sock, pkt, 14 + ip_total, 0) < 0) {
        perror("send icmp");
        return;
    }
    icmp_sent++;
    printf("        ICMP %s sent to ",
           type == ICMP_TIME_EXCEEDED ? "time exceeded" :
           code == 1 ? "host unreachable" : "network unreachable");
    print_ip(orig + 26);
    printf(" from ");
    print_ip(ifaces[in].ip);
    printf("\n");
}

/* ======================================================================
 * M9: FIREWALL (stateless, like a Cisco ACL)
 * Rules are checked top to bottom, the first match wins, default = DENY.
 * ====================================================================== */

#define PROTO_ANY  0
#define PROTO_ICMP 1
#define PROTO_TCP  6
#define PROTO_UDP  17
#define PORT_ANY   0

struct fw_rule {
    int           allow;          /* 1 = ALLOW, 0 = DENY */
    unsigned char protocol;       /* PROTO_ICMP, PROTO_TCP, PROTO_UDP or PROTO_ANY */
    unsigned char src_net[4];
    unsigned char src_mask[4];    /* 0.0.0.0 = any source */
    unsigned int  src_port;       /* PORT_ANY = any */
    unsigned char dst_net[4];
    unsigned char dst_mask[4];    /* 0.0.0.0 = any destination */
    unsigned int  dst_port;       /* PORT_ANY = any */
    const char   *text;           /* for printing */
    unsigned long hits;           /* how many packets matched this rule */
};

#define FW_RULE_COUNT 4
static struct fw_rule fw_rules[FW_RULE_COUNT] = {
    /* 1. ping and traceroute */
    { 1, PROTO_ICMP, {0,0,0,0},     {0,0,0,0},         PORT_ANY,
                     {0,0,0,0},     {0,0,0,0},         PORT_ANY, "ALLOW icmp any -> any", 0 },
    /* 2. pc-a (or anyone) can reach the web server on pc-b, port 8080 */
    { 1, PROTO_TCP,  {0,0,0,0},     {0,0,0,0},         PORT_ANY,
                     {10,0,2,10},   {255,255,255,255}, 8080,     "ALLOW tcp any -> 10.0.2.10:8080", 0 },
    /* 3. ...and its answers can come back (stateless: needs its own rule) */
    { 1, PROTO_TCP,  {10,0,2,10},   {255,255,255,255}, 8080,
                     {0,0,0,0},     {0,0,0,0},         PORT_ANY, "ALLOW tcp 10.0.2.10:8080 -> any", 0 },
    /* 4. telnet is blocked explicitly */
    { 0, PROTO_TCP,  {0,0,0,0},     {0,0,0,0},         PORT_ANY,
                     {0,0,0,0},     {0,0,0,0},         23,       "DENY  tcp any -> any:23 (telnet)", 0 },
};
static unsigned long fw_default_hits = 0;   /* packets that matched no rule */

/* ---- M9: is this IP inside net/mask? 1 = yes, 0 = no ---- */
static int ip_in_net(const unsigned char *ip, const unsigned char *net, const unsigned char *mask)
{
    for (int i = 0; i < 4; i++) {
        if ((ip[i] & mask[i]) != net[i]) {
            return 0;
        }
    }
    return 1;
}

/* ---- M9: read the ports of a TCP/UDP packet. 1 = ok, 0 = packet too short ---- */
static int read_ports(const unsigned char *frame, unsigned int header_size, size_t size,
                      unsigned int *src_port, unsigned int *dst_port)
{
    *src_port = 0;
    *dst_port = 0;
    if (frame[23] != PROTO_TCP && frame[23] != PROTO_UDP) {
        return 1;                                   /* no ports for ICMP: stay 0 */
    }
    unsigned int l4 = 14 + header_size;             /* where TCP/UDP starts (usually byte 34) */
    if (l4 + 4 > size) {
        return 0;                                   /* not enough bytes for the ports */
    }
    *src_port = frame[l4]     * 256 + frame[l4 + 1];
    *dst_port = frame[l4 + 2] * 256 + frame[l4 + 3];
    return 1;
}

/* ---- M9: check the rules. Returns the matching rule number, or -1 (default deny) ---- */
static int check_firewall(const unsigned char *frame, unsigned int header_size, size_t size)
{
    unsigned char protocol = frame[23];
    unsigned int src_port, dst_port;
    if (!read_ports(frame, header_size, size, &src_port, &dst_port)) {
        fw_default_hits++;
        return -1;
    }

    for (int r = 0; r < FW_RULE_COUNT; r++) {
        if (fw_rules[r].protocol != PROTO_ANY && fw_rules[r].protocol != protocol)       continue;
        if (!ip_in_net(frame + 26, fw_rules[r].src_net, fw_rules[r].src_mask))           continue;
        if (!ip_in_net(frame + 30, fw_rules[r].dst_net, fw_rules[r].dst_mask))           continue;
        if (fw_rules[r].src_port != PORT_ANY && fw_rules[r].src_port != src_port)        continue;
        if (fw_rules[r].dst_port != PORT_ANY && fw_rules[r].dst_port != dst_port)        continue;
        fw_rules[r].hits++;
        return r;                                   /* first match wins */
    }
    fw_default_hits++;
    return -1;                                      /* nothing matched: default deny */
}

/* ---- M5: receive one packet on door "in" and forward it if we should ---- */
static void handle_packet(int in)
{
    unsigned char frame[2048];
    struct sockaddr_ll from;
    socklen_t from_len = sizeof from;

    /* 1. Receive, and learn the direction of the packet */
    ssize_t size = recvfrom(ifaces[in].sock, frame, sizeof frame, 0,
                            (struct sockaddr *)&from, &from_len);
    if (size < 0) {
        return;
    }

    /* 2. Ignore the packets we sent ourselves (they pass by this door too) */
    if (from.sll_pkttype == PACKET_OUTGOING) {
        return;
    }

    /* 3. Only IPv4, long enough, and addressed to this door's MAC.
     *    Everything else (ARP, broadcasts) is left to Linux. */
    if (size < 34) return;
    if (frame[12] * 256 + frame[13] != 0x0800) return;
    if (memcmp(frame, ifaces[in].mac, 6) != 0) return;

    /* 4. Same safety checks as M3 */
    unsigned int version     = frame[14] / 16;
    unsigned int header_size = (frame[14] % 16) * 4;
    unsigned int total_size  = frame[16] * 256 + frame[17];
    if (version != 4 || header_size < 20 || 14 + header_size > (size_t)size ||
        total_size < header_size || 14 + total_size > (size_t)size) {
        drop(ifaces[in].name, frame, "bad IPv4 header");
        return;
    }
    if (ipv4_checksum(frame + 14, header_size) != frame[24] * 256u + frame[25]) {
        drop(ifaces[in].name, frame, "bad checksum");
        return;
    }

    /* 5. Packets for the router itself (e.g. ping 10.0.1.1) are for Linux, not forwarded */
    const unsigned char *dest = frame + 30;
    for (int i = 0; i < IFACE_COUNT; i++) {
        if (memcmp(dest, ifaces[i].ip, 4) == 0) {
            return;
        }
    }

    /* 5b. M9: firewall, before routing (no need to route a packet we will block) */
    int rule = check_firewall(frame, header_size, (size_t)size);
    if (rule == -1 || !fw_rules[rule].allow) {
        char reason[96];
        snprintf(reason, sizeof reason, "firewall: %s",
                 rule == -1 ? "default deny" : fw_rules[rule].text);
        drop(ifaces[in].name, frame, reason);       /* silent: no ICMP for blocked packets */
        return;
    }

    /* 6. Route lookup (M4) */
    int line = lookup_route(dest);
    if (line == -1) {
        drop(ifaces[in].name, frame, "no route");
        send_icmp_error(in, frame, header_size, total_size, ICMP_DEST_UNREACHABLE, 0);
        return;
    }
    int out = find_iface(routing_table[line].door);
    if (out == -1) {
        drop(ifaces[in].name, frame, "route to an unknown door");
        return;
    }

    /* 7. TTL */
    unsigned int ttl = frame[22];
    if (ttl <= 1) {
        drop(ifaces[in].name, frame, "TTL expired");
        send_icmp_error(in, frame, header_size, total_size, ICMP_TIME_EXCEEDED, 0);
        return;
    }

    /* 8. Next machine's MAC (static ARP) */
    const unsigned char *next_mac = find_neighbor_mac(dest);
    if (next_mac == NULL) {
        drop(ifaces[in].name, frame, "no MAC for this destination");
        send_icmp_error(in, frame, header_size, total_size, ICMP_DEST_UNREACHABLE, 1);
        return;
    }

    /* 9. Rewrite: TTL - 1, new checksum, new MACs */
    frame[22] = (unsigned char)(ttl - 1);
    unsigned int checksum = ipv4_checksum(frame + 14, header_size);
    frame[24] = (unsigned char)(checksum / 256);
    frame[25] = (unsigned char)(checksum % 256);
    memcpy(frame,     next_mac,         6);    /* destination MAC = next machine */
    memcpy(frame + 6, ifaces[out].mac,  6);    /* source MAC      = our outgoing door */

    /* 10. Send it out of the chosen door */
    if (send(ifaces[out].sock, frame, (size_t)size, 0) < 0) {
        perror("send");
        drop(ifaces[in].name, frame, "send failed");
        return;
    }

    forwarded++;
    printf("[%s -> %s]  ", ifaces[in].name, ifaces[out].name);
    print_ip(frame + 26);
    printf(" -> ");
    print_ip(dest);
    printf("  %s", protocol_name(frame[23]));
    unsigned int src_port, dst_port;
    if (frame[23] != PROTO_ICMP && read_ports(frame, header_size, (size_t)size, &src_port, &dst_port)) {
        printf(" %u -> %u", src_port, dst_port);
    }
    printf("  TTL %u -> %u  route %s  fw #%d\n",
           ttl, ttl - 1, routing_table[line].text, rule + 1);
}

/* ---- M5: router mode: open both doors, then forward until Ctrl+C ---- */
static int run_router(void)
{
    for (int i = 0; i < IFACE_COUNT; i++) {
        ifaces[i].sock = open_raw_socket(ifaces[i].name);
        if (ifaces[i].sock < 0) {
            return 1;
        }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) < 0) {
        perror("sigaction");
        return 1;
    }

    /* poll() watches both sockets and wakes up when one of them has a packet */
    struct pollfd watch[IFACE_COUNT];
    for (int i = 0; i < IFACE_COUNT; i++) {
        watch[i].fd     = ifaces[i].sock;
        watch[i].events = POLLIN;             /* "tell me when there is something to read" */
    }

    printf("Router running on eth0 and eth1... (Ctrl+C to stop)\n");

    while (!stop_requested) {
        int ready = poll(watch, IFACE_COUNT, -1);     /* -1 = wait as long as needed */
        if (ready < 0) {
            if (errno == EINTR) {
                continue;                             /* woken up by Ctrl+C */
            }
            perror("poll");
            break;
        }
        for (int i = 0; i < IFACE_COUNT; i++) {
            if (watch[i].revents & POLLIN) {          /* this door has a packet */
                handle_packet(i);
            }
        }
    }

    printf("\n--- Router statistics ---\n");
    printf("Forwarded : %lu\n", forwarded);
    printf("Dropped   : %lu\n", dropped);
    printf("ICMP sent : %lu\n", icmp_sent);

    printf("\n--- Firewall rules (hits) ---\n");
    for (int r = 0; r < FW_RULE_COUNT; r++) {
        printf(" #%d  %-36s %lu\n", r + 1, fw_rules[r].text, fw_rules[r].hits);
    }
    printf("     %-36s %lu\n", "DENY  everything else (default)", fw_default_hits);

    for (int i = 0; i < IFACE_COUNT; i++) {
        close(ifaces[i].sock);
    }
    return 0;
}

int main(int argc, char *argv[])
{
    /* ---- M5: router mode ---- */
    if (argc == 2 && strcmp(argv[1], "--forward") == 0) {
        return run_router();
    }
    /* ---- Step 1: which door to watch ---- */
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <interface>    (watch one door)\n"
                        "       %s --forward      (router mode, M5)\n", argv[0], argv[0]);
        return 1;
    }
    const char *interfaceName = argv[1];

    /* ---- Step 2: ask Linux for a pass (needs sudo) ---- */
    int screen = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (screen < 0) {
        perror("socket");
        return 1;
    }

    /* ---- Step 3: only this door ---- */
    unsigned int camera = if_nametoindex(interfaceName);
    if (camera == 0) {
        perror("if_nametoindex");
        close(screen);
        return 1;
    }

    struct sockaddr_ll form;
    memset(&form, 0, sizeof form);
    form.sll_family   = AF_PACKET;
    form.sll_protocol = htons(ETH_P_ALL);
    form.sll_ifindex  = (int)camera;

    if (bind(screen, (struct sockaddr *)&form, sizeof form) < 0) {
        perror("bind");
        close(screen);
        return 1;
    }

    /* ---- Step 4: box, counting paper, Ctrl+C rule ---- */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, NULL) < 0) {
        perror("sigaction");
        close(screen);
        return 1;
    }

    printf("Listening on %s... (Ctrl+C to stop)\n", interfaceName);

    unsigned char frame[2048];
    struct stats stats;
    memset(&stats, 0, sizeof stats);

    /* ---- Step 5: the loop ---- */
    while (!stop_requested) {

        /* A. Wait for a packet */
        ssize_t size = recv(screen, frame, sizeof frame, 0);

        /* B. Woken up by Ctrl+C, or a real error? */
        if (size < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("recv");
            break;
        }

        /* C. Count it, check the size */
        stats.total++;
        stats.bytes += (unsigned long long)size;

        if (size < ETH_HEADER_LEN) {
            stats.too_short++;
            printf("#%lu  too short (%zd bytes), skipped\n", stats.total, size);
            continue;
        }

        /* D. Read to whom (box 0), from whom (box 6), type (boxes 12-13) */
        const unsigned char *destination = frame;
        const unsigned char *source      = frame + 6;
        unsigned int type = frame[12] * 256 + frame[13];

        /* E. Count by type */
        if (type == 0x0800)      stats.ipv4++;
        else if (type == 0x0806) stats.arp++;
        else if (type == 0x86DD) stats.ipv6++;
        else                     stats.other++;

        /* F. Print the Ethernet line */
        printf("#%lu  %zd bytes  ", stats.total, size);
        print_mac(source);
        printf(" -> ");
        print_mac(destination);
        printf("  %s (0x%04x)\n", ethertype_name(type), type);

        /* G. M3: if it's IPv4, print the IP line too */
        if (type == 0x0800) {
            print_ipv4(frame, (size_t)size);
        }
    }

    /* ---- Step 6: the end ---- */
    print_stats(&stats, interfaceName);
    close(screen);
    return 0;
}