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

// PING
typedef struct {
    u8 msgno;
    String alias;
    String group;
    String pingtext;
} PingMsg;

void broadcast_whosthere(Arena scratch);
gpointer THREAD_wait_for_udp_messages(gpointer data);
gpointer THREAD_wait_for_tcp_messages(gpointer data);

char *GBindPort = BIND_PORT;

int main(int argc, char *argv[]) {
    Arena scratch = ArenaNew(1024);

    gtk_init(&argc, &argv);

    if (argc > 1)
        GBindPort = argv[1];

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

#define SIN_ADDR(sa) ( (void *) &((struct sockaddr_in *)sa)->sin_addr )
#define SIN6_ADDR(sa) ( (void *) &((struct sockaddr_in6 *)sa)->sin6_addr )
char *get_ipaddr(struct sockaddr *sa) {
    static char ipaddr[INET6_ADDRSTRLEN+1];
    void *sin_addr = sa->sa_family == AF_INET ? SIN_ADDR(sa) : SIN6_ADDR(sa);
    if (inet_ntop(sa->sa_family, sin_addr, ipaddr, sizeof(ipaddr)) == NULL) {
        fprintf(stderr, "inet_ntop(): %s\n", strerror(errno));
        return "";
    }
    return ipaddr;
}
void get_ipaddr2(struct sockaddr *sa, String *outstr) {
    char ipaddr[INET6_ADDRSTRLEN+1];
    void *sin_addr = sa->sa_family == AF_INET ? SIN_ADDR(sa) : SIN6_ADDR(sa);
    if (inet_ntop(sa->sa_family, sin_addr, ipaddr, sizeof(ipaddr)) == NULL) {
        fprintf(stderr, "inet_ntop(): %s\n", strerror(errno));
        StringAssign(outstr, "");
        return;
    }
    StringAssign(outstr, ipaddr);
}

// HostAddr combines IPv4 address (sin_addr 32 bits) + network port (sin_port 16 bits)
// hostaddr = (sin_port << 32) + sin_addr
typedef u64 HostAddr;

in_port_t sockaddr_port_from_hostaddr(HostAddr hostaddr) {
    return (in_port_t) (hostaddr >> 32);
}
struct in_addr sockaddr_addr_from_hostaddr(HostAddr hostaddr) {
    struct in_addr sin_addr;
    sin_addr.s_addr = (u32) (hostaddr & 0x00000000FFFFFFFF);
    return sin_addr;
}

// Conversion from HostAddr <--> sockaddr_in
HostAddr hostaddr_from_sockaddr(struct sockaddr_in *sa) {
    return ((u64) sa->sin_port << 32) + sa->sin_addr.s_addr;
}
struct sockaddr_in sockaddr_in_from_hostaddr(HostAddr hostaddr) {
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = sockaddr_port_from_hostaddr(hostaddr);
    sa.sin_addr = sockaddr_addr_from_hostaddr(hostaddr);
    return sa;
}

void get_ipaddr3(HostAddr hostaddr, String *outstr) {
    char ipaddr[INET6_ADDRSTRLEN+1];
    struct in_addr sin_addr = sockaddr_addr_from_hostaddr(hostaddr);
    if (inet_ntop(AF_INET, &sin_addr, ipaddr, sizeof(ipaddr)) == NULL) {
        fprintf(stderr, "inet_ntop(): %s\n", strerror(errno));
        StringAssign(outstr, "");
        return;
    }
    StringAssign(outstr, ipaddr);
}
char * get_ipaddr4(HostAddr hostaddr) {
    static char ipaddr[INET6_ADDRSTRLEN+1];
    struct in_addr sin_addr = sockaddr_addr_from_hostaddr(hostaddr);
    if (inet_ntop(AF_INET, &sin_addr, ipaddr, sizeof(ipaddr)) == NULL) {
        fprintf(stderr, "inet_ntop(): %s\n", strerror(errno));
        return "";
    }
    return ipaddr;
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
        exit(1);
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
            struct sockaddr_storage fromaddr;
            socklen_t fromaddr_len = sizeof(fromaddr);
            z = recvfrom(hostfd, buf, sizeof(buf), 0, (struct sockaddr *)&fromaddr, &fromaddr_len);
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

                if (((struct sockaddr *)&fromaddr)->sa_family == AF_INET) {
                    struct sockaddr_in *sa = (struct sockaddr_in *) &fromaddr;
                    HostAddr hostaddr = hostaddr_from_sockaddr(sa);
                    printf("hostaddr: %llx ipaddr: '%s' port: %d\n", hostaddr, get_ipaddr4(hostaddr), ntohs(sockaddr_port_from_hostaddr(hostaddr)));
                }
            }
        }
    }

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
        exit(1);
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

    fd_set readfds, writefds;
    fd_set tmp_readfds, tmp_writefds;
    FD_ZERO(&readfds);
    FD_ZERO(&writefds);
    FD_SET(hostfd, &readfds);
    int maxfd = hostfd;

    Arena tscratch = ArenaNew(255);

    while (1) {
        ArenaReset(&tscratch);
        tmp_readfds = readfds;
        tmp_writefds = writefds;

        printf("Listening for TCP messages on port %s...\n", GBindPort);
        z = select(maxfd+1, &tmp_readfds, &tmp_writefds, NULL, NULL);
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


