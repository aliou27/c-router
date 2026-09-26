#define _DEFAULT_SOURCE

#include <arpa/inet.h>
#include <errno.h>               /* errno, EINTR */
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <signal.h>              /* sigaction, SIGINT, sig_atomic_t */
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define ETH_HEADER_LEN 14

/* ---- The tally sheet: one counter per category ---- */
struct stats {
    unsigned long      total;
    unsigned long      ipv4;
    unsigned long      arp;
    unsigned long      ipv6;
    unsigned long      other;
    unsigned long      too_short;
    unsigned long long bytes;
};

/* ---- The flag: raised by the signal handler, read by the loop ---- */
static volatile sig_atomic_t stop_requested = 0;

/* ---- Signal handler: only raises the flag ---- */
static void on_sigint(int signum)
{
    (void)signum;                /* unused parameter, silences a warning */
    stop_requested = 1;
}

/* ---- Helper: print 6 bytes as aa:bb:cc:dd:ee:ff ---- */
static void print_mac(const unsigned char *mac)
{
    for (int i = 0; i < 6; i++) {
        printf("%02x", mac[i]);
        if (i < 5) {
            printf(":");
        }
    }
}

/* ---- Helper: turn a type number into a name ---- */
static const char *ethertype_name(unsigned int type)
{
    if (type == 0x0800) return "IPv4";
    if (type == 0x0806) return "ARP";
    if (type == 0x86DD) return "IPv6";
    return "other";
}

/* ---- Print the summary ---- */
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
    /* ---- 0. Check the input ---- */
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <interface>\n", argv[0]);
        return 1;
    }
    const char *interfaceName = argv[1];

    /* ---- 1. Open a raw socket ---- */
    int screen = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (screen < 0) {
        perror("socket");
        return 1;
    }

    /* ---- 2. Find the interface number ---- */
    unsigned int camera = if_nametoindex(interfaceName);
    if (camera == 0) {
        perror("if_nametoindex");
        close(screen);
        return 1;
    }

    /* ---- 3. Bind to that interface only ---- */
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

    /* ---- NEW: on Ctrl+C, call on_sigint instead of dying ---- */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;             /* no SA_RESTART: recv must be woken up */

    if (sigaction(SIGINT, &sa, NULL) < 0) {
        perror("sigaction");
        close(screen);
        return 1;
    }

    printf("Listening on %s... (Ctrl+C to stop)\n", interfaceName);

    unsigned char frame[2048];
    struct stats stats;
    memset(&stats, 0, sizeof stats);   /* all counters start at 0 */

    /* ---- 4. Receive loop, until the flag is raised ---- */
    while (!stop_requested) {
        ssize_t size = recv(screen, frame, sizeof frame, 0);

        if (size < 0) {
            if (errno == EINTR) {
                continue;        /* woken by a signal: go check the flag */
            }
            perror("recv");      /* a real error */
            break;
        }

        stats.total++;
        stats.bytes += (unsigned long long)size;

        /* ---- 5a. Safety check ---- */
        if (size < ETH_HEADER_LEN) {
            stats.too_short++;
            printf("#%lu  too short (%zd bytes), skipped\n", stats.total, size);
            continue;
        }

        /* ---- 5b. Read the Ethernet header ---- */
        const unsigned char *destination = frame;
        const unsigned char *source      = frame + 6;
        unsigned int type = ((unsigned int)frame[12] << 8) | frame[13];

        /* ---- 5c. Count by type ---- */
        if (type == 0x0800)      stats.ipv4++;
        else if (type == 0x0806) stats.arp++;
        else if (type == 0x86DD) stats.ipv6++;
        else                     stats.other++;

        /* ---- 5d. Display ---- */
        printf("#%lu  %zd bytes  ", stats.total, size);
        print_mac(source);
        printf(" -> ");
        print_mac(destination);
        printf("  %s (0x%04x)\n", ethertype_name(type), type);
    }

    /* ---- 6. Summary and close (now reachable) ---- */
    print_stats(&stats, interfaceName);
    close(screen);
    return 0;
}