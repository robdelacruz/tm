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
#include "tmcommon.h"

#define TRACKER_HOST "127.0.0.1"
#define TRACKER_PORT 9000

void parse_args(int argc, char **argv);

void* THREAD_wait_for_tcp_messages(void *data);
void handle_msg(Arena scratch, int fd, HostAddr fromaddr, char *msgbytes, u16 msglen, Array *socketctxs, fd_set *writefds, int *maxfd);

String GTrackerHost;
u16 GTrackerPort = TRACKER_PORT;

int main(int argc, char *argv[]) {
    Arena arena = ArenaNew(64*1024);
    Arena scratch = ArenaNew(1024);

    GTrackerHost = StringNew(&arena, TRACKER_HOST);
    GTrackerPort = TRACKER_PORT;

    parse_args(argc, argv);

    pthread_t thread_wait_tcp;
    pthread_create(&thread_wait_tcp, NULL, THREAD_wait_for_tcp_messages, NULL);

    pthread_join(thread_wait_tcp, NULL);

    return 0;
}

enum ParseArgs {SW_NONE, SW_TRACKERPORT};
void parse_args(int argc, char **argv) {
    enum ParseArgs state = SW_NONE;

    for (int i=0; i < argc; i++) {
        char *arg = argv[i];
        if (state == SW_NONE) {
            if (CSTR_EQUALS(arg, "-trackerport"))
                state = SW_TRACKERPORT;
            continue;
        }

        if (state == SW_TRACKERPORT)
            GTrackerPort = atoi(arg);
        state = SW_NONE;
    }
}

void* THREAD_wait_for_tcp_messages(void *data) {
    int z;
    int listenfd = OpenTcpSocket(CSTR(GTrackerHost), GTrackerPort);
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
    Arena socketctx_arena = ArenaNew(1024*1024);
    Array socketctxs = ArrayNew(&socketctx_arena, 64, sizeof(SocketCtx));

    printf("Tracker listening for messages on %s/%d...\n", CSTR(GTrackerHost), GTrackerPort);
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

                    SocketCtx_new(socketfd, &socketctxs, HostAddrFromSockAddr((struct sockaddr_in *) &ss), 0, 64, 1);
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
                                handle_msg(tscratch, socketfd, socketctx->fromaddr, readbuf->bs, socketctx->msglen, &socketctxs, &writefds, &maxfd);
                                
                                BufferShift(readbuf, socketctx->msglen);
                                socketctx->msglen = 0;
                                continue;
                            }
                            break;
                        }
                    }
                    if (read_eof) {
                        FD_CLR(socketfd, &readfds);
                        shutdown(socketfd, SHUT_RD);
                        socketctx->shut_rd = 1;

                        if (socketctx->shut_rd && socketctx->writebuf.len == 0) {
                            SocketCtx_close_and_remove(socketctx, &socketctxs);

                            if (socketctxs.len == 0) {
                                ArenaReset(socketctxs.arena);
                                socketctxs = ArrayNew(socketctxs.arena, 64, sizeof(SocketCtx));
                            }
                            printf("Closed socketfd %d\n", socketfd);
                        }
                    }
                }
            }
            if (FD_ISSET(i, &tmp_writefds)) {
                int socketfd = i;

                SocketCtx *socketctx = SocketCtx_find_by_fd(socketctxs, socketfd);
                if (socketctx == NULL) {
                    fprintf(stderr, "socketfd %d not found in socketctxs\n", socketfd);
                    continue;
                }

                z = NetSend2(socketctx->fd, &socketctx->writebuf, &writefds, &maxfd);
                if (z == 0) {
                    FD_CLR(socketfd, &writefds);
                    shutdown(socketfd, SHUT_WR);
                }
                if (z == -1) {
                    FD_CLR(socketfd, &writefds);
                    shutdown(socketfd, SHUT_WR);
                    BufferClear(&socketctx->writebuf);
                }

                if (socketctx->shut_rd && socketctx->writebuf.len == 0) {
                    SocketCtx_close_and_remove(socketctx, &socketctxs);

                    if (socketctxs.len == 0) {
                        ArenaReset(socketctxs.arena);
                        socketctxs = ArrayNew(socketctxs.arena, 64, sizeof(SocketCtx));
                    }
                    printf("Closed socketfd %d\n", socketfd);
                }

            }

        }
    }

    ShutdownSocket(listenfd);
    ArenaFree(&socketctx_arena);
    ArenaFree(&tscratch);
}

void handle_msg(Arena scratch, int fd, HostAddr fromaddr, char *msgbytes, u16 msglen, Array *socketctxs, fd_set *writefds, int *maxfd) {
    u8 msgno = MSGNO(msgbytes);
    if (msgno == KNOCK) {
        String alias = StringNew0(&scratch);
        String hostname = StringNew0(&scratch);
        u16 listenport;
        NetUnpack(msgbytes, msglen, "%b%s%s%w", &msgno, &alias, &hostname, &listenport);

        printf("** KNOCK alias: %s' hostname: '%s' listenport: %d received from %s/%d **\n", CSTR(alias), CSTR(hostname), listenport, HostAddr_ipaddress(fromaddr), ntohs(HostAddr_port(fromaddr)));

        // toaddr has the same ip address as fromaddr, but with listenport as its port.
        struct sockaddr_in to_sa = SockAddrFromHostAddr(fromaddr);
        to_sa.sin_port = htons(listenport);
        HostAddr toaddr = HostAddrFromSockAddr(&to_sa);

        // Inform all peers of new peer. 
        Buffer sendbuf = BufferNew(&scratch, 1024);
        NetPackLen(&sendbuf, "%b%s%s%L%L", PEER_ONLINE, CSTR(alias), CSTR(hostname), fromaddr, toaddr);
        send_msg_to_peers(scratch, sendbuf.bs, sendbuf.len, socketctxs, writefds, maxfd);

        // Inform new peer of existing peers.
        Array peers = get_peers_array();
        for (int i=0; i < peers.len; i++) {
            Peer *p = ArrayItem(peers, i);
            if (p->active == 0)
                continue;

            BufferClear(&sendbuf);
            NetPackLen(&sendbuf, "%b%s%s%L%L", PEER_ONLINE, p->alias, p->hostname, p->fromaddr, p->toaddr);
            send_msg_to_hostaddr(scratch, sendbuf.bs, sendbuf.len, toaddr, socketctxs, writefds, maxfd);
        }

        create_peer(CSTR(alias), CSTR(hostname), fromaddr, toaddr);
        print_peers();
    } else if (msgno == BYE) {
        printf("** BYE ** received from %s/%d **\n", HostAddr_ipaddress(fromaddr), ntohs(HostAddr_port(fromaddr)));
        TMHandle hpeer = find_peer_fromaddr(fromaddr);
        if (hpeer != -1)
            destroy_peer(hpeer);
        print_peers();

        // Inform all peers that this peer is going offline.
        Buffer sendbuf = BufferNew(&scratch, 1024);
        NetPackLen(&sendbuf, "%b%L", PEER_OFFLINE, fromaddr);
        send_msg_to_peers(scratch, sendbuf.bs, sendbuf.len, socketctxs, writefds, maxfd);

    }
}

