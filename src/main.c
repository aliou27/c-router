#define _DEFAULT_SOURCE

#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define ETH_HEADER_LEN 14

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

    printf("Listening on %s...\n", interfaceName);

    unsigned char frame[2048];
    unsigned long count = 0;

    /* ---- 4. Receive loop ---- */
    for (;;) {
        ssize_t size = recv(screen, frame, sizeof frame, 0);
        if (size < 0) {
            perror("recv");
            break;
        }

        count++;

        /* ---- 5a. Safety check: is there a full Ethernet header? ---- */
        if (size < ETH_HEADER_LEN) {
            printf("#%lu  too short (%zd bytes), skipped\n", count, size);
            continue;
        }

        /* ---- 5b. Read the Ethernet header ---- */
        const unsigned char *destination = frame;        /* bytes 0-5  */
        const unsigned char *source      = frame + 6;    /* bytes 6-11 */
        unsigned int type = ((unsigned int)frame[12] << 8) | frame[13];

        /* ---- 5c. Display ---- */
        printf("#%lu  %zd bytes  ", count, size);
        print_mac(source);
        printf(" -> ");
        print_mac(destination);
        printf("  %s (0x%04x)\n", ethertype_name(type), type);
    }

    /* ---- 6. Close ---- */
    close(screen);
    return 0;
}