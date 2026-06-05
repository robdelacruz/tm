#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <signal.h>
#include <limits.h>
#include <time.h>
#include <pthread.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "clib.h"
#include "cnet.h"

#define TRACKER_HOST "127.0.0.1"
#define TRACKER_PORT "8001"
#define BIND_PORT "9001"

#define MSGNO(bs) (*((u8 *)bs))
#define PING 1

// PING
typedef struct {
    u8 msgno;
    String alias;
    String hostname;
    HostAddr hostaddr_from;
    String text;
} PingMsg;

typedef struct {
    String alias;
    String hostname;
    HostAddr hostaddr;
} Peer;

typedef struct {
    int fd;
    HostAddr hostaddr;
    Buffer readbuf;
    Buffer writebuf;
    u16 msglen;
    String alias;
    String hostname;
} SocketCtx;

void* THREAD_wait_for_tcp_messages(void *data);

SocketCtx *SocketCtx_find_by_fd(Array ctxs, int fd);
int SocketCtx_find_by_fd2(Array ctxs, int fd);
void handle_msg(Arena scratch, int fd, HostAddr hostaddr, char *msgbytes, u16 msglen);
void Peer_add_or_replace(Array *peers, Peer peer);
void Peer_remove(Array *peers, HostAddr hostaddr);

char *GBindPort = BIND_PORT;
Arena GArena, GScratch;
String GAlias;
String GHostname;
Array GPeers;

int main(int argc, char *argv[]) {
    GArena = ArenaNew(64*1024);
    GScratch = ArenaNew(1024);

    if (argc >= 2)
        GBindPort = argv[1];

    char *alias = getlogin();
    if (alias == NULL)
        alias = "noname";
    GAlias = StringNew(&GArena, alias);

    char hostname[HOST_NAME_MAX];
    int z = gethostname(hostname, sizeof(hostname));
    if (z == -1) {
        fprintf(stderr, "gethostname() %s\n", strerror(errno));
        exit(1);
    }
    hostname[HOST_NAME_MAX-1] = 0;
    GHostname = StringNew(&GArena, hostname);

    printf("BindPort: %s\n", GBindPort);
    printf("Alias: %s\n", CSTR(GAlias));
    printf("Hostname: %s\n", CSTR(GHostname));

    GPeers = ArrayNew(&GArena, 64, sizeof(Peer));

    pthread_t thread_wait_tcp;
    pthread_create(&thread_wait_tcp, NULL, THREAD_wait_for_tcp_messages, NULL);

    pthread_join(thread_wait_tcp, NULL);

    return 0;
}

void* THREAD_wait_for_tcp_messages(void *data) {
    int z;
    int listenfd = OpenTcpSocket(NULL, GBindPort);
    if (listenfd == -1)
        return NULL;

    int backlog=50;
    z = listen(listenfd, backlog);
    if (z == -1) {
        fprintf(stderr, "listen(): %s\n", strerror(errno));
        return NULL;
    }

    fd_set readfds, tmp_readfds;
    fd_set writefds, tmp_writefds;
    FD_ZERO(&readfds);
    FD_ZERO(&writefds);
    FD_SET(listenfd, &readfds);
    int maxfd = listenfd;

    Arena tscratch = ArenaNew(1024*1024);
    Array socketctxs = ArrayNew(&tscratch, 64, sizeof(SocketCtx));

    printf("Listening for messages on port %s...\n", GBindPort);
    while (1) {
        tmp_readfds = readfds;
        tmp_writefds = writefds;

        z = select(maxfd+1, &tmp_readfds, &tmp_writefds, NULL, NULL);
        if (z == 0) // timeout
            continue;
        if (z == -1 && errno == EINTR)
            continue;
        if (z == -1) {
            fprintf(stderr, "select(): %s\n", strerror(errno));
            return NULL;
        }

        for (int i=0; i <= maxfd; i++) {
            if (FD_ISSET(i, &tmp_readfds)) {
                if (i == listenfd) {
                    // New peer connection
                    struct sockaddr_storage ss;
                    socklen_t ss_len = sizeof(ss);
                    int socketfd = accept(listenfd, (struct sockaddr *) &ss, &ss_len);
                    if (socketfd == -1) {
                        fprintf(stderr, "accept(): %s\n", strerror(errno));
                        continue;
                    }

                    // Only accept IPv4 connections
                    if (ss.ss_family != AF_INET) {
                        printf("Ignoring non-IPv4 fd %d\n", socketfd);
                        ShutdownSocket(socketfd);
                        continue;
                    }

                    SocketCtx socketctx;
                    socketctx.fd = socketfd;
                    socketctx.hostaddr = HostAddrFromSockAddr((struct sockaddr_in *)&ss);
                    socketctx.readbuf = BufferNew(&tscratch, 64);
                    socketctx.writebuf = BufferNew(&tscratch, 64);
                    socketctx.msglen = 0;
                    socketctx.alias = StringNew0(&tscratch);
                    socketctx.hostname = StringNew0(&tscratch);
                    ArrayAppend(&socketctxs, &socketctx);

                    FD_SET(socketfd, &readfds);
                    if (socketfd > maxfd)
                        maxfd = socketfd;

                    printf("New socketfd: %d\n", socketfd);
                } else {
                    // Received bytes from peer
                    int socketfd = i;
                    SocketCtx *socketctx = SocketCtx_find_by_fd(socketctxs, socketfd);
                    if (socketctx == NULL) {
                        fprintf(stderr, "socketfd %d not found in socketctxs\n", socketfd);
                        continue;
                    }

                    int read_eof = 0;
                    if (NetRecv(socketfd, &socketctx->readbuf) == 0)
                        read_eof = 1;

                    Buffer *readbuf = &socketctx->readbuf;
                    while (1) {
                        if (socketctx->msglen == 0) {
                            // Read msglen
                            if (readbuf->len >= sizeof(u16)) {
                                u16 *bs = (u16 *) readbuf->bs;
                                socketctx->msglen = ntohs(*bs);
                                if (socketctx->msglen == 0) {
                                    read_eof = 1;
                                    break;
                                }
                                BufferShift(readbuf, sizeof(u16));
                                continue;
                            }
                            break;
                        } else {
                            // Read msg body (msglen bytes)
                            if (readbuf->len >= socketctx->msglen) {
                                handle_msg(tscratch, socketfd, socketctx->hostaddr, readbuf->bs, socketctx->msglen);
                                
                                BufferShift(readbuf, socketctx->msglen);
                                socketctx->msglen = 0;
                                continue;
                            }
                            break;
                        }
                    }
                    if (read_eof) {
                        FD_CLR(socketfd, &readfds);
                        ShutdownSocket(socketfd);

                        int index = SocketCtx_find_by_fd2(socketctxs, socketfd);
                        ArrayRemove(&socketctxs, index);
                        if (socketctxs.len == 0) {
                            ArenaReset(&tscratch);
                            socketctxs = ArrayNew(&tscratch, 64, sizeof(SocketCtx));
                        }
                        printf("Closed socketfd %d\n", socketfd);
                    }
                }
            }
        }
    }

    shutdown(listenfd, SHUT_RDWR);
    close(listenfd);
}

SocketCtx *SocketCtx_find_by_fd(Array ctxs, int fd) {
    for (int i=0; i < ctxs.len; i++) {
        SocketCtx *ctx = ArrayItem(ctxs, i);
        if (ctx->fd == fd)
            return ctx;
    }
    return NULL;
}
int SocketCtx_find_by_fd2(Array ctxs, int fd) {
    for (int i=0; i < ctxs.len; i++) {
        SocketCtx *ctx = ArrayItem(ctxs, i);
        if (ctx->fd == fd)
            return i;
    }
    return -1;
}

void handle_msg(Arena scratch, int fd, HostAddr hostaddr, char *msgbytes, u16 msglen) {
    u8 msgno = MSGNO(msgbytes);
    if (msgno == PING) {
        String alias = StringNew0(&scratch);
        String hostname = StringNew0(&scratch);
        HostAddr hostaddr_from;
        String text = StringNew0(&scratch);
        NetUnpack(msgbytes, msglen, "%b%s%s%L%s", &msgno, &alias, &hostname, &hostaddr_from, &text);
        printf("** PING alias: '%s' hostname: '%s' hostaddr_from: %s (port %d) text: '%s' **\n", CSTR(alias), CSTR(hostname), HostAddr_ipaddress(hostaddr_from), ntohs(HostAddr_port(hostaddr_from)), CSTR(text));

        if (StringEquals(text, "knock")) {
            // Tracker forwarded 'knock', source address is in hostaddr_from 
            Peer peer;
            peer.hostaddr = hostaddr_from;
            peer.alias = StringDup(GPeers.arena, alias);
            peer.hostname = StringDup(GPeers.arena, hostname);
            Peer_add_or_replace(&GPeers, peer);
        } else if (StringEquals(text, "hello")) {
            // Peer sends 'hello' directly
            Peer peer;
            peer.hostaddr = hostaddr;
            peer.alias = StringDup(GPeers.arena, alias);
            peer.hostname = StringDup(GPeers.arena, hostname);
            Peer_add_or_replace(&GPeers, peer);
        } else if (StringEquals(text, "bye")) {
            // Tracker forwarded 'bye', source addressed is in hostaddr_from
            Peer_remove(&GPeers, hostaddr_from);
        }
    }
}

void Peer_add_or_replace(Array *peers, Peer peer) {
    // Replace if a peer with the same hostaddr exists
    for (int i=0; i < peers->len; i++) {
        Peer *p = ArrayItem(*peers, i);
        if (p->hostaddr == peer.hostaddr) {
            ArrayReplace(peers, i, &peer);
            return;
        }
    }
    ArrayAppend(peers, &peer);
}

void Peer_remove(Array *peers, HostAddr hostaddr) {
    for (int i=0; i < peers->len; i++) {
        Peer *p = ArrayItem(*peers, i);
        if (p->hostaddr == hostaddr) {
            ArrayRemove(peers, i);
            return;
        }
    }
}

