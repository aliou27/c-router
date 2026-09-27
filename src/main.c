#define _DEFAULT_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
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

int main(int argc, char *argv[])
{
    /* ---- Step 1: which door to watch ---- */
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <interface>\n", argv[0]);
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