#define _DEFAULT_SOURCE          /* unlocks Linux socket functions with -std=c17 */

#include <arpa/inet.h>           /* htons */
#include <linux/if_ether.h>      /* ETH_P_ALL */
#include <linux/if_packet.h>     /* struct sockaddr_ll */
#include <net/if.h>              /* if_nametoindex */
#include <stdio.h>               /* printf, perror */
#include <string.h>              /* memset */
#include <sys/socket.h>          /* socket, bind, recv */
#include <unistd.h>              /* close */

int main(int argc, char *argv[])
{

    printf("C Router starting...\n");

    /* ---- 0. Check the input ---- */
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <interface>\n", argv[0]);
        return 1;
    }
    const char *interfaceName = argv[1];

    /* ---- 1. Ask the chief for a screen (socket) ---- */
    int screen = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (screen < 0) {
        perror("socket");
        return 1;
    }

    /* ---- 2. Find the camera number (interface index) ---- */
    unsigned int camera = if_nametoindex(interfaceName);
    if (camera == 0) {
        perror("if_nametoindex");
        close(screen);
        return 1;
    }

    /* ---- 3. Connect the screen to that camera only (bind) ---- */
    struct sockaddr_ll form;
    memset(&form, 0, sizeof form);             /* all fields to zero */
    form.sll_family   = AF_PACKET;             /* raw packet */
    form.sll_protocol = htons(ETH_P_ALL);      /* all protocols */
    form.sll_ifindex  = (int)camera;           /* this interface */

    if (bind(screen, (struct sockaddr *)&form, sizeof form) < 0) {
        perror("bind");
        close(screen);
        return 1;
    }

    printf("Listening on %s...\n", interfaceName);

    unsigned char frame[2048];                 /* the buffer */
    unsigned long count = 0;

    /* ---- 4. Watch forever (receive loop) ---- */
    for (;;) {
        ssize_t size = recv(screen, frame, sizeof frame, 0);  /* sleeps here */
        if (size < 0) {
            perror("recv");
            break;
        }

        count++;

        /* ---- 5. Look at the photo ---- */
        printf("#%lu  %zd bytes  ", count, size);
        for (ssize_t i = 0; i < size && i < 16; i++) {
            printf("%02x ", frame[i]);
        }
        printf("\n");
    }

    /* ---- 6. Give the screen back (close) ---- */
    close(screen);
    return 0;
}