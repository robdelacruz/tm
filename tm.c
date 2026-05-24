#include <gtk/gtk.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <signal.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "clib.h"
#include "cnet.h"

#define BIND_PORT "8002"
#define UDP_LISTEN_PORT "8001"

#define MSGNO(bs) (*((u8 *)bs))
#define PING 1

// HostAddr combines IPv4 address (sin_addr 32 bits) + network port (sin_port 16 bits)
// hostaddr = (sin_port << 32) + sin_addr
typedef u64 HostAddr;

HostAddr hostaddr_from_sockaddr(struct sockaddr_in *sa);
struct sockaddr_in sockaddr_from_hostaddr(HostAddr hostaddr);
char * get_ip_address(HostAddr hostaddr);
void get_ip_address2(HostAddr hostaddr, String *outstr);

// PING
typedef struct {
    u8 msgno;
    String alias;
    String group;
    String pingtext;
} PingMsg;

struct addrinfo *get_self_addrinfo(char *port);
int sockaddr_matches_addrinfo(struct sockaddr *sa, struct addrinfo *ai);

void broadcast_whosthere(Arena scratch);
gpointer THREAD_wait_for_udp_messages(gpointer data);
gpointer THREAD_wait_for_tcp_messages(gpointer data);
int connect_and_send_message(struct sockaddr *sa_dest, Buffer *sendbuf, struct timeval *timeout_val);

char *GBindPort = BIND_PORT;
char *GMyAlias = "rob";
char *GMyGroup = "bsdg";
struct addrinfo *GMyAddrInfo=NULL;

int main(int argc, char *argv[]) {
    Arena scratch = ArenaNew(1024);

    gtk_init(&argc, &argv);

    if (argc > 1)
        GBindPort = argv[1];

    GMyAddrInfo = get_self_addrinfo(GBindPort);

    GtkWidget *w = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(w), 275,425);
    gtk_window_set_position(GTK_WINDOW(w), GTK_WIN_POS_CENTER);
    gtk_window_set_title(GTK_WINDOW(w), "TinyMsg");
    g_signal_connect(w, "destroy", G_CALLBACK(gtk_main_quit), NULL);

//    gtk_widget_show_all(w);

    g_thread_new("", THREAD_wait_for_udp_messages, NULL);
    g_thread_new("", THREAD_wait_for_tcp_messages, NULL);
    broadcast_whosthere(scratch);

    gtk_main();
    return 0;
}

// Conversion from HostAddr <--> sockaddr_in
HostAddr hostaddr_from_sockaddr(struct sockaddr_in *sa) {
    return ((u64) sa->sin_port << 32) + sa->sin_addr.s_addr;
}
static in_port_t sockaddr_port_from_hostaddr(HostAddr hostaddr) {
    return (in_port_t) (hostaddr >> 32);
}
static struct in_addr sockaddr_addr_from_hostaddr(HostAddr hostaddr) {
    struct in_addr sin_addr;
    sin_addr.s_addr = (u32) (hostaddr & 0x00000000FFFFFFFF);
    return sin_addr;
}
struct sockaddr_in sockaddr_from_hostaddr(HostAddr hostaddr) {
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = sockaddr_port_from_hostaddr(hostaddr);
    sa.sin_addr = sockaddr_addr_from_hostaddr(hostaddr);
    return sa;
}

char * get_ip_address(HostAddr hostaddr) {
    static char ipaddr[INET6_ADDRSTRLEN+1];
    struct in_addr sin_addr = sockaddr_addr_from_hostaddr(hostaddr);
    if (inet_ntop(AF_INET, &sin_addr, ipaddr, sizeof(ipaddr)) == NULL) {
        fprintf(stderr, "inet_ntop(): %s\n", strerror(errno));
        return "";
    }
    return ipaddr;
}
void get_ip_address2(HostAddr hostaddr, String *outstr) {
    char ipaddr[INET6_ADDRSTRLEN+1];
    struct in_addr sin_addr = sockaddr_addr_from_hostaddr(hostaddr);
    if (inet_ntop(AF_INET, &sin_addr, ipaddr, sizeof(ipaddr)) == NULL) {
        fprintf(stderr, "inet_ntop(): %s\n", strerror(errno));
        StringAssign(outstr, "");
        return;
    }
    StringAssign(outstr, ipaddr);
}

struct addrinfo *get_self_addrinfo(char *port) {
    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;

    struct addrinfo *hostai;
    int z = getaddrinfo(NULL, port, &hints, &hostai);
    if (z != 0) {
        fprintf(stderr, "getaddrinfo(): %s\n", gai_strerror(z));
        exit(1);
    }
    return hostai;
}
// Only ipv4 supported
int sockaddr_matches_addrinfo(struct sockaddr *sa, struct addrinfo *ai) {
    if (sa->sa_family != AF_INET)
        return 0;
    for (struct addrinfo *p = ai; p != NULL; p = p->ai_next) {
        if (p->ai_family != AF_INET)
            continue;
        struct sockaddr_in *sa1 = (struct sockaddr_in *) sa;
        struct sockaddr_in *sa2 = (struct sockaddr_in *) p->ai_addr;
        printf("sa1 s_addr: %d sa2 s_addr: %d\n", sa1->sin_addr.s_addr, sa2->sin_addr.s_addr);
        if (sa1->sin_addr.s_addr == sa2->sin_addr.s_addr && sa1->sin_port == sa2->sin_port)
            return 1;
    }
    return 0;
}


void broadcast_whosthere(Arena scratch) {
    int z;
    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;

    struct addrinfo *hostai;
    z = getaddrinfo(NULL, GBindPort, &hints, &hostai);
    if (z != 0) {
        fprintf(stderr, "getaddrinfo(): %s\n", gai_strerror(z));
        exit(1);
    }

    int hostfd = socket(hostai->ai_family, hostai->ai_socktype, hostai->ai_protocol);
    if (hostfd == -1) {
        fprintf(stderr, "socket(): %s\n", strerror(errno));
        exit(1);
    }
    z = bind(hostfd, hostai->ai_addr, hostai->ai_addrlen);
    if (z == -1) {
        fprintf(stderr, "broadcast_whosthere bind(): %s\n", strerror(errno));
        exit(1);
    }
    freeaddrinfo(hostai);

    int yes=1;
    z = setsockopt(hostfd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    if (z == -1) {
        fprintf(stderr, "setsockopt(SO_BROADCAST): %s\n", strerror(errno));
        exit(1);
    }

    struct addrinfo *broadcastai;
    z = getaddrinfo("255.255.255.255", UDP_LISTEN_PORT, &hints, &broadcastai);
    if (z != 0) {
        fprintf(stderr, "getaddrinfo(): %s\n", gai_strerror(z));
        exit(1);
    }

    Buffer buf = BufferNew(&scratch, 32);
    NetPack(&buf, "%b%s%s%s", PING, "rob", "bsdg", "whosthere");
    z = sendto(hostfd, buf.bs, buf.len, 0, broadcastai->ai_addr, sizeof(struct sockaddr));
    if (z == -1) {
        fprintf(stderr, "broadcast_whosthere() sendto(): %s\n", strerror(errno));
        exit(1);
    }

    freeaddrinfo(broadcastai);

    close(hostfd);
}

gpointer THREAD_wait_for_udp_messages(gpointer data) {
    int z;
    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;

    struct addrinfo *hostai;
    z = getaddrinfo(NULL, UDP_LISTEN_PORT, &hints, &hostai);
    if (z != 0) {
        fprintf(stderr, "getaddrinfo(): %s\n", gai_strerror(z));
        exit(1);
    }

    int hostfd = socket(hostai->ai_family, hostai->ai_socktype, hostai->ai_protocol);
    if (hostfd == -1) {
        fprintf(stderr, "socket(): %s\n", strerror(errno));
        exit(1);
    }
    z = bind(hostfd, hostai->ai_addr, hostai->ai_addrlen);
    if (z == -1) {
        fprintf(stderr, "wait_for_udp bind(): %s\n", strerror(errno));
        fprintf(stderr, "Unable to receive UDP messages.\n");
        freeaddrinfo(hostai);
        return NULL;
    }
    freeaddrinfo(hostai);

    int yes=1;
    z = setsockopt(hostfd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    if (z == -1) {
        fprintf(stderr, "setsockopt(SO_BROADCAST): %s\n", strerror(errno));
        exit(1);
    }

    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(hostfd, &readfds);

    Arena tscratch = ArenaNew(255);

    while (1) {
        ArenaReset(&tscratch);

        printf("Listening for UDP messages...\n");
        z = select(hostfd+1, &readfds, NULL, NULL, NULL);
        if (z == -1 && errno == EINTR)
            continue;
        if (z == -1) {
            fprintf(stderr, "select(): %s\n", strerror(errno));
            abort();
        }

        if (FD_ISSET(hostfd, &readfds)) {
            char buf[256];
            struct sockaddr_storage sa_storage;
            struct sockaddr *sa_peer = (struct sockaddr *) &sa_storage;
            socklen_t sa_peer_len = sizeof(sa_storage);
            z = recvfrom(hostfd, buf, sizeof(buf), 0, sa_peer, &sa_peer_len);
            if (z == -1) {
                fprintf(stderr, "recvfrom(): %s\n", strerror(errno));
                continue;
            }

            u8 msgno = MSGNO(buf);
            if (msgno == PING) {
                String alias = StringNew0(&tscratch);
                String group = StringNew0(&tscratch);
                String pingtext = StringNew0(&tscratch);
                NetUnpack(buf, z, "%b%s%s%s", &msgno, &alias, &group, &pingtext);
                printf("** PING alias: '%s' group: '%s' pingtext: '%s' **\n", CSTR(alias), CSTR(group), CSTR(pingtext));

                if (sa_peer->sa_family != AF_INET) {
                    fprintf(stderr, "Ignoring non-IPV4 UDP message.\n");
                    continue;
                }

                HostAddr hostaddr = hostaddr_from_sockaddr((struct sockaddr_in *)sa_peer);
                printf("hostaddr: %llx ipaddr: '%s' port: %d\n", hostaddr, get_ip_address(hostaddr), ntohs(sockaddr_port_from_hostaddr(hostaddr)));

                // Ignore messages broadcast by myself
                if (sockaddr_matches_addrinfo(sa_peer, GMyAddrInfo)) {
                    fprintf(stderr, "Ignoring UDP message sent by myself.\n");
                    continue;
                }

                /*
                if (StringEquals(pingtext, "whosthere")) {
                    ArenaReset(&tscratch);
                    Buffer writebuf = BufferNew(&tscratch, 64);
                    NetPack(&writebuf, "%b%s%s%s", PING, GMyAlias, GMyGroup, "hello");
                    struct timeval timeout_val = {2, 0};
                    fprintf(stderr, "Responding 'hello' to 'whosthere'\n");
                    connect_and_send_message(sa_peer, &writebuf, &timeout_val);
                }
                */
            }
        }
    }

}

int connect_and_send_message(struct sockaddr *sa_dest, Buffer *sendbuf, struct timeval *timeout_val) {
    int z;

    // Connect to destination
    int destfd = socket(AF_INET, SOCK_STREAM, 0);
    if (destfd == -1) {
        fprintf(stderr, "connect_and_send_message socket(): %s\n", strerror(errno));
        return -1;
    }
    int yes=1;
    z = setsockopt(destfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (z == -1) {
        fprintf(stderr, "setsockopt(): %s\n", strerror(errno));
        return -1;
    }
    z = connect(destfd, sa_dest, sizeof(struct sockaddr));
    if (z == -1) {
        fprintf(stderr, "connect_and_send_message connect(): %s\n", strerror(errno));
        return -1;
    }

    z = NetSend(destfd, sendbuf);
    if (z == -1) {
        return -1;
    }
    if (z == 0)
        return 0;

    fd_set writefds;
    FD_ZERO(&writefds);
    FD_SET(destfd, &writefds);

    while (1) {
        z = select(destfd+1, NULL, &writefds, NULL, timeout_val);
        if (z == 0) {
            fprintf(stderr, "connect_and_send_message select() timeout\n");
            return -1;
        }
        if (z == -1 && errno == EINTR)
            continue;
        if (z == -1) {
            fprintf(stderr, "connect_and_send_message select(): %s\n", strerror(errno));
            return -1;
        }
        if (FD_ISSET(destfd, &writefds)) {
            z = NetSend(destfd, sendbuf);
            if (z == 0)
                break;
            if (z == -1) {
                fprintf(stderr, "connect_and_send_message() network error during send\n");
                return -1;
            }
        }
    }
    return 0;
}

gpointer THREAD_wait_for_tcp_messages(gpointer data) {
    int z;
    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    struct addrinfo *hostai;
    z = getaddrinfo(NULL, GBindPort, &hints, &hostai);
    if (z != 0) {
        fprintf(stderr, "getaddrinfo(): %s\n", gai_strerror(z));
        exit(1);
    }

    int hostfd = socket(hostai->ai_family, hostai->ai_socktype, hostai->ai_protocol);
    if (hostfd == -1) {
        fprintf(stderr, "socket(): %s\n", strerror(errno));
        exit(1);
    }
    z = bind(hostfd, hostai->ai_addr, hostai->ai_addrlen);
    if (z == -1) {
        fprintf(stderr, "bind(): %s\n", strerror(errno));
        fprintf(stderr, "Unable to receive TCP messages.\n");
        freeaddrinfo(hostai);
        return NULL;
    }
    freeaddrinfo(hostai);

    int backlog=50;
    z = listen(hostfd, backlog);
    if (z == -1) {
        fprintf(stderr, "listen(): %s\n", strerror(errno));
        exit(1);
    }

    int yes=1;
    z = setsockopt(hostfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (z == -1) {
        fprintf(stderr, "setsockopt(SO_BROADCAST): %s\n", strerror(errno));
        exit(1);
    }

    fd_set readfds, tmp_readfds;
    FD_ZERO(&readfds);
    FD_SET(hostfd, &readfds);
    int maxfd = hostfd;

    Arena tscratch = ArenaNew(255);

    while (1) {
        ArenaReset(&tscratch);
        tmp_readfds = readfds;

//        printf("Listening for TCP messages on port %s...\n", GBindPort);
        z = select(maxfd+1, &tmp_readfds, NULL, NULL, NULL);
        if (z == 0) // timeout
            continue;
        if (z == -1 && errno == EINTR)
            continue;
        if (z == -1) {
            fprintf(stderr, "select(): %s\n", strerror(errno));
            abort();
        }

        for (int i=0; i <= maxfd; i++) {
            if (FD_ISSET(hostfd, &tmp_readfds)) {
                if (i == hostfd) {
                    // New peer connection
                    socklen_t sa_len = sizeof(struct sockaddr_in);
                    struct sockaddr_in sa;
                    int peerfd = accept(hostfd, (struct sockaddr *) &sa, &sa_len);
                    if (peerfd == -1) {
                        fprintf(stderr, "accept(): %s\n", strerror(errno));
                        continue;
                    }
                    FD_SET(peerfd, &readfds);
                    if (peerfd > maxfd)
                        maxfd = peerfd;

                    //todo Add peer to peer list
                } else {
                    // Received bytes from peer
                }
            }
        }
    }

}


