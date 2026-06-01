#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <signal.h>
#include <limits.h>
#include <crypt.h>
#include <time.h>
#include <pthread.h>

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

// PING
typedef struct {
    u8 msgno;
    String alias;
    String hostname;
    String pingtext;
} PingMsg;

typedef struct {
    int peerfd;
    HostAddr hostaddr;
    Buffer readbuf;
    u16 msglen;
    String alias;
    String hostname;
} PeerCtx;

typedef struct {
    HostAddr hostaddr;
    String alias;
    String hostname;
} PeerNode;

HostAddr HostAddrFromSockAddr(struct sockaddr_in *sa);
struct in_addr HostAddr_addr(HostAddr hostaddr);
in_port_t HostAddr_port(HostAddr hostaddr);
struct sockaddr_in SockAddrFromHostAddr(HostAddr hostaddr);

char *get_ip_address(HostAddr hostaddr);
void get_ip_address2(HostAddr hostaddr, String *outstr);

void broadcast_whosthere(Arena scratch);
void* THREAD_wait_for_udp_messages(void *data);
void* THREAD_wait_for_tcp_messages(void *data);
int connect_and_send_message(struct sockaddr *sa_dest, Buffer *sendbuf, struct timeval *timeout_val);

String GetSignature(Arena *arena, Arena scratch, String alias, String hostname);
PeerCtx *find_peerctx(Array peerctxs, int peerfd);
int find_peerctx_index(Array peerctxs, int peerfd);
void process_peer_msg(Arena scratch, int peerfd, HostAddr hostaddr, char *msgbytes, u16 msglen);
void add_or_replace_peernode(Array *peernodes, PeerNode peernode);
void remove_peernode(Array *peernodes, HostAddr hostaddr);

char *GBindPort = BIND_PORT;
Arena GArena, GScratch;
String GAlias;
String GHostname;
Array GPeernodes;

int main(int argc, char *argv[]) {
    GArena = ArenaNew(64*1024);
    GScratch = ArenaNew(1024);

    if (argc >= 2)
        GBindPort = argv[1];

    if (argc >= 3) {
        GAlias = StringNew(&GArena, argv[2]);
    } else {
        char *alias = getlogin();
        if (alias == NULL)
            alias = "noname";
        GAlias = StringNew(&GArena, alias);
    }

    char buf[HOST_NAME_MAX];
    int z = gethostname(buf, sizeof(buf));
    if (z == -1) {
        fprintf(stderr, "gethostname() %s\n", strerror(errno));
        exit(1);
    }
    buf[HOST_NAME_MAX-1] = 0;
    GHostname = StringNew(&GArena, buf);
    GPeernodes = ArrayNew(&GArena, 64, sizeof(PeerNode));

    printf("Port: %s\n", GBindPort);
    printf("Alias: %s\n", CSTR(GAlias));
    printf("Hostname: %s\n", CSTR(GHostname));

    GtkWidget *w = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(w), 275,425);
    gtk_window_set_position(GTK_WINDOW(w), GTK_WIN_POS_CENTER);
    gtk_window_set_title(GTK_WINDOW(w), "TinyMsg");
    g_signal_connect(w, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    pthread_t thread_wait_udp, thread_wait_tcp;
    pthread_create(&thread_wait_udp, THREAD_wait_for_udp_messages, NULL);
    g_thread_new(&thread_wait_tcp, THREAD_wait_for_tcp_messages, NULL);
    broadcast_whosthere(GScratch);

    return 0;
}

// Conversion from HostAddr <--> sockaddr_in
HostAddr HostAddrFromSockAddr(struct sockaddr_in *sa) {
    return ((u64) sa->sin_port << 32) + sa->sin_addr.s_addr;
}
struct in_addr HostAddr_addr(HostAddr hostaddr) {
    struct in_addr sin_addr;
    sin_addr.s_addr = (u32) (hostaddr & 0x00000000FFFFFFFF);
    return sin_addr;
}
in_port_t HostAddr_port(HostAddr hostaddr) {
    return (in_port_t) (hostaddr >> 32);
}
struct sockaddr_in SockAddrFromHostAddr(HostAddr hostaddr) {
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = HostAddr_port(hostaddr);
    sa.sin_addr = HostAddr_addr(hostaddr);
    return sa;
}

char *get_ip_address(HostAddr hostaddr) {
    static char ipaddr[INET6_ADDRSTRLEN+1];
    struct in_addr sin_addr = HostAddr_addr(hostaddr);
    if (inet_ntop(AF_INET, &sin_addr, ipaddr, sizeof(ipaddr)) == NULL) {
        fprintf(stderr, "inet_ntop(): %s\n", strerror(errno));
        return "";
    }
    return ipaddr;
}
void get_ip_address2(HostAddr hostaddr, String *outstr) {
    char ipaddr[INET6_ADDRSTRLEN+1];
    struct in_addr sin_addr = HostAddr_addr(hostaddr);
    if (inet_ntop(AF_INET, &sin_addr, ipaddr, sizeof(ipaddr)) == NULL) {
        fprintf(stderr, "inet_ntop(): %s\n", strerror(errno));
        StringAssign(outstr, "");
        return;
    }
    StringAssign(outstr, ipaddr);
}

String GetSignature(Arena *arena, Arena scratch, String alias, String hostname) {
    static char *CRYPTSALT = "salt1234567890";
    srand(time(NULL));
    String phrase = StringFormat(&scratch, "%s%s%d", CSTR(GAlias), CSTR(GHostname), rand());
    if (phrase.len > CRYPT_MAX_PASSPHRASE_SIZE)
        phrase.bs[CRYPT_MAX_PASSPHRASE_SIZE] = 0;

    struct crypt_data data;
    memset(&data, 0, sizeof(data));
    char *pz = crypt_r(CSTR(phrase), CRYPTSALT, &data);
    assert(pz != NULL);

    return StringNew(arena, data.output);
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
    NetPack(&buf, "%b%s%s%s", PING, CSTR(GAlias), CSTR(GHostname), "whosthere");
    z = sendto(hostfd, buf.bs, buf.len, 0, broadcastai->ai_addr, sizeof(struct sockaddr));
    if (z == -1) {
        fprintf(stderr, "broadcast_whosthere() sendto(): %s\n", strerror(errno));
        exit(1);
    }

    freeaddrinfo(broadcastai);

    close(hostfd);
}

void* THREAD_wait_for_udp_messages(void *data) {
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

    Arena tscratch = ArenaNew(1024);

    printf("Listening for UDP messages on port %s...\n", UDP_LISTEN_PORT);
    while (1) {
        ArenaReset(&tscratch);

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
                String hostname = StringNew0(&tscratch);
                String pingtext = StringNew0(&tscratch);
                NetUnpack(buf, z, "%b%s%s%s", &msgno, &alias, &hostname, &pingtext);
                printf("** PING alias: '%s' hostname: '%s' pingtext: '%s' **\n", CSTR(alias), CSTR(hostname), CSTR(pingtext));

                if (sa_peer->sa_family != AF_INET) {
                    fprintf(stderr, "Ignoring non-IPV4 UDP message.\n");
                    continue;
                }

                // Ignore messages broadcast by myself
                if (StringEquals(alias, CSTR(GAlias)) && StringEquals(hostname, CSTR(GHostname)))
                    continue;

                if (StringEquals(pingtext, "whosthere")) {
                    Buffer writebuf = BufferNew(&tscratch, 64);
                    NetPack(&writebuf, "%b%s%s%s", PING, CSTR(GAlias), CSTR(GHostname), "hello");
                    struct timeval timeout_val = {2, 0};
                    fprintf(stderr, "Responding 'hello' to 'whosthere'\n");
                    connect_and_send_message(sa_peer, &writebuf, &timeout_val);
                }
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

void* THREAD_wait_for_tcp_messages(void *data) {
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

    Arena tscratch = ArenaNew(1024*1024);
    Array peerctxs = ArrayNew(&tscratch, 64, sizeof(PeerCtx));

    printf("Listening for TCP messages on port %s...\n", GBindPort);
    while (1) {
        tmp_readfds = readfds;

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
            if (FD_ISSET(i, &tmp_readfds)) {
                if (i == hostfd) {
                    // New peer connection
                    struct sockaddr_storage ss;
                    socklen_t ss_len = sizeof(ss);
                    int peerfd = accept(hostfd, (struct sockaddr *) &ss, &ss_len);
                    if (peerfd == -1) {
                        fprintf(stderr, "accept(): %s\n", strerror(errno));
                        continue;
                    }

                    // Only accept IPv4 connections
                    if (ss.ss_family != AF_INET) {
                        printf("Ignoring non-IPv4 fd %d\n", peerfd);
                        shutdown(peerfd, SHUT_RDWR);
                        close(peerfd);
                        continue;
                    }

                    // Add peer to peer list
                    PeerCtx peerctx;
                    peerctx.peerfd = peerfd;
                    peerctx.hostaddr = HostAddrFromSockAddr((struct sockaddr_in *)&ss);
                    peerctx.readbuf = BufferNew(&tscratch, 64);
                    peerctx.msglen = 0;
                    peerctx.alias = StringNew0(&tscratch);
                    peerctx.hostname = StringNew0(&tscratch);
                    ArrayAppend(&peerctxs, &peerctx);

                    FD_SET(peerfd, &readfds);
                    if (peerfd > maxfd)
                        maxfd = peerfd;

                    printf("New peerctx fd: %d\n", peerctx.peerfd);
                } else {
                    // Received bytes from peer
                    int peerfd = i;
                    PeerCtx *peerctx = find_peerctx(peerctxs, peerfd);
                    if (peerctx == NULL) {
                        fprintf(stderr, "peerfd %d not found in peerctxs\n", peerfd);
                        continue;
                    }

                    int read_eof = 0;
                    if (NetRecv(peerfd, &peerctx->readbuf) == 0)
                        read_eof = 1;

                    Buffer *readbuf = &peerctx->readbuf;
                    while (1) {
                        if (peerctx->msglen == 0) {
                            // Read msglen
                            if (readbuf->len >= sizeof(u16)) {
                                u16 *bs = (u16 *) readbuf->bs;
                                peerctx->msglen = ntohs(*bs);
                                if (peerctx->msglen == 0) {
                                    read_eof = 1;
                                    break;
                                }
                                BufferShift(readbuf, sizeof(u16));
                                continue;
                            }
                            break;
                        } else {
                            // Read msg body (msglen bytes)
                            if (readbuf->len >= peerctx->msglen) {
                                process_peer_msg(tscratch, peerfd, peerctx->hostaddr, readbuf->bs, peerctx->msglen);
                                
                                BufferShift(readbuf, peerctx->msglen);
                                peerctx->msglen = 0;
                                continue;
                            }
                            break;
                        }
                    }
                    if (read_eof) {
                        FD_CLR(peerfd, &readfds);
                        shutdown(peerfd, SHUT_RDWR);
                        close(peerfd);

                        int ipeer = find_peerctx_index(peerctxs, peerfd);
                        ArrayRemove(&peerctxs, ipeer);
                        if (peerctxs.len == 0) {
                            ArenaReset(&tscratch);
                            peerctxs = ArrayNew(&tscratch, 64, sizeof(PeerCtx));
                        }
                        printf("Closed peer %d\n", peerfd);
                    }
                }
            }
        }
    }

}

PeerCtx *find_peerctx(Array peerctxs, int peerfd) {
    for (int i=0; i < peerctxs.len; i++) {
        PeerCtx *peerctx = ArrayItem(peerctxs, i);
        if (peerctx->peerfd == peerfd)
            return peerctx;
    }
    return NULL;
}
int find_peerctx_index(Array peerctxs, int peerfd) {
    for (int i=0; i < peerctxs.len; i++) {
        PeerCtx *peerctx = ArrayItem(peerctxs, i);
        if (peerctx->peerfd == peerfd)
            return i;
    }
    return -1;
}

void process_peer_msg(Arena scratch, int peerfd, HostAddr hostaddr, char *msgbytes, u16 msglen) {
    u8 msgno = MSGNO(msgbytes);
    if (msgno == PING) {
        String alias = StringNew0(&scratch);
        String hostname = StringNew0(&scratch);
        String pingtext = StringNew0(&scratch);
        NetUnpack(msgbytes, msglen, "%b%s%s%s", &msgno, &alias, &hostname, &pingtext);
        printf("** PING alias: '%s' hostname: '%s' pingtext: '%s' **\n", CSTR(alias), CSTR(hostname), CSTR(pingtext));

        if (StringEquals(pingtext, "hello")) {
            struct sockaddr_in sa;
            socklen_t sa_len = sizeof(sa);
            int z = getsockname(peerfd, (struct sockaddr *)&sa, &sa_len);
            if (z == -1) {
                fprintf(stderr, "process_peer_msg getsockname(): %s\n", strerror(z));
                return;
            }
            HostAddr hostaddr = HostAddrFromSockAddr(&sa);
            printf("'hello' received from IP %s port %d\n", get_ip_address(hostaddr), HostAddr_port(hostaddr));

            PeerNode peernode;
            peernode.hostaddr = hostaddr;
            peernode.alias = StringDup(GPeernodes.arena, alias);
            peernode.hostname = StringDup(GPeernodes.arena, hostname);
            add_or_replace_peernode(&GPeernodes, peernode);
        }
    }
}

void add_or_replace_peernode(Array *peernodes, PeerNode peernode) {
    // Replace peernode if a node with the same hostaddr exists
    for (int i=0; i < peernodes->len; i++) {
        PeerNode *p = ArrayItem(*peernodes, i);
        if (p->hostaddr == peernode.hostaddr) {
            ArrayReplace(peernodes, i, &peernode);
            return;
        }
    }
    ArrayAppend(peernodes, &peernode);
}

void remove_peernode(Array *peernodes, HostAddr hostaddr) {
    for (int i=0; i < peernodes->len; i++) {
        PeerNode *p = ArrayItem(*peernodes, i);
        if (p->hostaddr == hostaddr) {
            ArrayRemove(peernodes, i);
            return;
        }
    }
}

