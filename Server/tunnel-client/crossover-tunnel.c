/*
 * crossover-tunnel -- player-side half of the Crossover dedicated-server bridge.
 *
 * Fable's multiplayer mod speaks SLikeNet, which is UDP-only. Railway (and most
 * PaaS hosts) will only forward TCP. This carries the mod's UDP datagrams over a
 * single TCP connection to the server, which unwraps them back into UDP locally.
 *
 *     Fable + EgoMP.dll  --UDP 127.0.0.1:60000-->  this  --TCP-->  server
 *
 * Run it, leave it running, then in-game press NUMPAD 2 and connect to
 * 127.0.0.1 on the local port below. Nothing in the mod needs to change.
 *
 * Framing on the TCP side is [uint16 big-endian length][datagram], matching
 * Server/src/Tunnel.cpp.
 *
 * Build (from Server/):
 *   cc     -O2 -o crossover-tunnel        tunnel-client/crossover-tunnel.c
 *   i686-w64-mingw32-gcc -O2 -static -o crossover-tunnel.exe \
 *          tunnel-client/crossover-tunnel.c -lws2_32
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
   typedef int socklen_t;
#  define CLOSESOCK closesocket
#  define SOCKERR   WSAGetLastError()
#else
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <unistd.h>
#  include <errno.h>
   typedef int SOCKET;
#  define INVALID_SOCKET (-1)
#  define CLOSESOCK close
#  define SOCKERR   errno
#endif

#define MAX_DATAGRAM 2048
#define FRAME_HEADER 2
#define DEFAULT_LOCAL_PORT 60000

static void die(const char *what) {
    fprintf(stderr, "crossover-tunnel: %s (error %d)\n", what, SOCKERR);
    exit(1);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,
            "crossover-tunnel -- play Fable multiplayer through a dedicated server\n"
            "\n"
            "usage: %s <server-host> <server-tcp-port> [local-udp-port] [bind-address]\n"
            "\n"
            "  server-host       host given by your server operator\n"
            "                    (on Railway: the TCP Proxy domain, e.g. shuttle.proxy.rlwy.net)\n"
            "  server-tcp-port   port given by your server operator\n"
            "  local-udp-port    port to expose locally (default %d)\n"
            "  bind-address      interface to expose it on (default 127.0.0.1;\n"
            "                    use 0.0.0.0 only to share the tunnel on a trusted LAN)\n"
            "\n"
            "Leave this running, then in-game press NUMPAD 2 and enter:\n"
            "  IP: 127.0.0.1     Port: <local-udp-port>\n",
            argv[0], DEFAULT_LOCAL_PORT);
        return 2;
    }

    const char *host = argv[1];
    const char *port = argv[2];
    unsigned short localPort =
        (argc > 3) ? (unsigned short)atoi(argv[3]) : DEFAULT_LOCAL_PORT;
    /* Loopback by default: the game runs on the same machine. Override only to
       share one tunnel with other machines on a trusted LAN. */
    const char *bindAddr = (argc > 4) ? argv[4] : "127.0.0.1";

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) die("WSAStartup failed");
#endif

    /* Resolve and connect to the server. */
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0 || !res) {
        fprintf(stderr, "crossover-tunnel: cannot resolve %s:%s\n", host, port);
        return 1;
    }

    SOCKET tcp = INVALID_SOCKET;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        tcp = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (tcp == INVALID_SOCKET) continue;
        if (connect(tcp, ai->ai_addr, (socklen_t)ai->ai_addrlen) == 0) break;
        CLOSESOCK(tcp);
        tcp = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (tcp == INVALID_SOCKET) die("could not connect to the server");

    int one = 1;
    setsockopt(tcp, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));

    /* Listen for the game on loopback. */
    SOCKET udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp == INVALID_SOCKET) die("could not create the local UDP socket");

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    if (strcmp(bindAddr, "0.0.0.0") == 0) {
        local.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, bindAddr, &local.sin_addr) != 1) {
        fprintf(stderr, "crossover-tunnel: '%s' is not an IPv4 address\n", bindAddr);
        return 1;
    }
    local.sin_port = htons(localPort);
    if (bind(udp, (struct sockaddr *)&local, sizeof(local)) != 0) {
        fprintf(stderr,
                "crossover-tunnel: could not bind %s:%u -- is another copy "
                "already running, or is the game hosting on that port?\n",
                bindAddr, localPort);
        return 1;
    }

    printf("crossover-tunnel: connected to %s:%s\n", host, port);
    printf("crossover-tunnel: in-game press NUMPAD 2, then IP %s  Port %u\n",
           strcmp(bindAddr, "0.0.0.0") == 0 ? "<this machine's IP>" : bindAddr, localPort);
    printf("crossover-tunnel: leave this window open while you play.\n");
    fflush(stdout);

    struct sockaddr_in game;
    memset(&game, 0, sizeof(game));
    int gameKnown = 0;

    unsigned char frame[MAX_DATAGRAM + FRAME_HEADER];
    unsigned char inbound[MAX_DATAGRAM * 4];
    size_t inboundLen = 0;

    for (;;) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(tcp, &rd);
        FD_SET(udp, &rd);
        SOCKET maxFd = (tcp > udp) ? tcp : udp;

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        int ready = select((int)maxFd + 1, &rd, NULL, NULL, &tv);
        if (ready < 0) {
#ifndef _WIN32
            if (errno == EINTR) continue;
#endif
            die("select failed");
        }
        if (ready == 0) continue;

        /* Game -> server. */
        if (FD_ISSET(udp, &rd)) {
            struct sockaddr_in from;
            socklen_t fromLen = sizeof(from);
            int n = recvfrom(udp, (char *)frame + FRAME_HEADER, MAX_DATAGRAM, 0,
                             (struct sockaddr *)&from, &fromLen);
            if (n > 0) {
                if (!gameKnown) {
                    game = from;
                    gameKnown = 1;
                    printf("crossover-tunnel: game connected from port %u\n",
                           ntohs(from.sin_port));
                    fflush(stdout);
                }
                frame[0] = (unsigned char)((n >> 8) & 0xFF);
                frame[1] = (unsigned char)(n & 0xFF);
                size_t total = (size_t)n + FRAME_HEADER, sent = 0;
                while (sent < total) {
                    int w = send(tcp, (const char *)frame + sent, (int)(total - sent), 0);
                    if (w <= 0) { fprintf(stderr, "crossover-tunnel: server closed the connection\n"); return 1; }
                    sent += (size_t)w;
                }
            }
        }

        /* Server -> game. */
        if (FD_ISSET(tcp, &rd)) {
            int n = recv(tcp, (char *)inbound + inboundLen,
                         (int)(sizeof(inbound) - inboundLen), 0);
            if (n == 0) { fprintf(stderr, "crossover-tunnel: server closed the connection\n"); return 1; }
            if (n < 0) { die("lost the server connection"); }
            inboundLen += (size_t)n;

            while (inboundLen >= FRAME_HEADER) {
                size_t len = ((size_t)inbound[0] << 8) | inbound[1];
                if (len == 0 || len > MAX_DATAGRAM) {
                    fprintf(stderr, "crossover-tunnel: bad frame from server\n");
                    return 1;
                }
                if (inboundLen < FRAME_HEADER + len) break;
                if (gameKnown) {
                    sendto(udp, (const char *)inbound + FRAME_HEADER, (int)len, 0,
                           (struct sockaddr *)&game, sizeof(game));
                }
                memmove(inbound, inbound + FRAME_HEADER + len,
                        inboundLen - (FRAME_HEADER + len));
                inboundLen -= FRAME_HEADER + len;
            }
        }
    }
}
