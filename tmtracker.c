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
#define TRACKER_PORT "8001"

void* THREAD_wait_for_tcp_messages(void *data);
void handle_msg(Arena scratch, int fd, HostAddr hostaddr, char *msgbytes, u16 msglen, Array *socketctxs, fd_set *writefds, int *maxfd);
void send_msg_to_peers(Arena scratch, char *msgbytes, u16 msglen, Array peers, Array *socketctxs, fd_set *writefds, int *maxfd);

Arena GArena, GScratch;
Array GPeers;

int main(int argc, char *argv[]) {
    GArena = ArenaNew(64*1024);
    GScratch = ArenaNew(1024);

    GPeers = ArrayNew(&GArena, 64, sizeof(Peer));

    pthread_t thread_wait_tcp;
    pthread_create(&thread_wait_tcp, NULL, THREAD_wait_for_tcp_messages, NULL);

    pthread_join(thread_wait_tcp, NULL);

    return 0;
}

void* THREAD_wait_for_tcp_messages(void *data) {
    int z;
    int listenfd = OpenTcpSocket(TRACKER_HOST, TRACKER_PORT);
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

    Arena tarena = ArenaNew(1024*1024);
    Arena tscratch = ArenaNew(1024*1024);
    Array socketctxs = ArrayNew(&tarena, 64, sizeof(SocketCtx));

    printf("Tracker listening for messages on port %s...\n", TRACKER_PORT);
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
                    socketctx.readbuf = BufferNew(&tarena, 64);
                    socketctx.writebuf = BufferNew(&tarena, 64);
                    socketctx.msglen = 0;
                    ArrayAppend(&socketctxs, &socketctx);

                    FD_SET(socketfd, &readfds);
                    if (socketfd > maxfd)
                        maxfd = socketfd;
                    //printf("New socketfd: %d\n", socketfd);
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
                                handle_msg(tscratch, socketfd, socketctx->hostaddr, readbuf->bs, socketctx->msglen, &socketctxs, &writefds, &maxfd);
                                
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
                            ArenaReset(&tarena);
                            socketctxs = ArrayNew(&tarena, 64, sizeof(SocketCtx));
                        }
                        //printf("Closed socketfd %d\n", socketfd);
                    }
                }
            }
        }
    }

    ShutdownSocket(listenfd);
    ArenaFree(&tarena);
    ArenaFree(&tscratch);
}

void handle_msg(Arena scratch, int fd, HostAddr hostaddr, char *msgbytes, u16 msglen, Array *socketctxs, fd_set *writefds, int *maxfd) {
    u8 msgno = MSGNO(msgbytes);
    if (msgno == KNOCK) {
        String alias = StringNew0(&scratch);
        String hostname = StringNew0(&scratch);
        u16 hostport;
        NetUnpack(msgbytes, msglen, "%b%s%s%w", &msgno, &alias, &hostname, &hostport);
        printf("** KNOCK alias: %s' hostname: '%s' hostport: %d received from %s/%d **\n", CSTR(alias), CSTR(hostname), hostport, HostAddr_ipaddress(hostaddr), ntohs(HostAddr_port(hostaddr)));

        struct sockaddr_in sa = SockAddrFromHostAddr(hostaddr);
        sa.sin_port = htons(hostport);
        HostAddr source_hostaddr = HostAddrFromSockAddr(&sa);

        Buffer sendbuf = BufferNew(&scratch, 1024);
        NetPackLen(&sendbuf, "%b%s%s%L%s", PING, CSTR(alias), CSTR(hostname), source_hostaddr, "hello");
        send_msg_to_peers(scratch, sendbuf.bs, sendbuf.len, GPeers, socketctxs, writefds, maxfd);

        Peer peer;
        peer.hostaddr = source_hostaddr;
        peer.alias = StringDup(GPeers.arena, alias);
        peer.hostname = StringDup(GPeers.arena, hostname);
        Peer_add_or_replace(&GPeers, peer);
        print_peers(GPeers);

    } else if (msgno == PING) {
        String alias = StringNew0(&scratch);
        String hostname = StringNew0(&scratch);
        HostAddr hostaddr_from;
        String text = StringNew0(&scratch);
        NetUnpack(msgbytes, msglen, "%b%s%s%L%s", &msgno, &alias, &hostname, &hostaddr_from, &text);
        printf("** PING alias: '%s' hostname: '%s' hostaddr_from: %s/%d) text: '%s' **\n", CSTR(alias), CSTR(hostname), HostAddr_ipaddress(hostaddr_from), ntohs(HostAddr_port(hostaddr_from)), CSTR(text));

        Buffer sendbuf = BufferNew(&scratch, 1024);

        if (StringEquals(text, "knock")) {
            // Received 'knock', forward to all peers.
            NetPackLen(&sendbuf, "%b%s%s%L%s", PING, CSTR(alias), CSTR(hostname), hostaddr, CSTR(text));
            send_msg_to_peers(scratch, sendbuf.bs, sendbuf.len, GPeers, socketctxs, writefds, maxfd);

            // Then add sender as a peer.
            Peer peer;
            peer.hostaddr = hostaddr;
            peer.alias = StringDup(GPeers.arena, alias);
            peer.hostname = StringDup(GPeers.arena, hostname);
            Peer_add_or_replace(&GPeers, peer);

            print_peers(GPeers);
        } else if (StringEquals(text, "bye")) {
            // Peer sent 'bye' directly
            Peer_remove(&GPeers, hostaddr);

            //todo: Forward bye to all peers
        }
    }
}

void send_msg_to_peers(Arena scratch, char *msgbytes, u16 msglen, Array peers, Array *socketctxs, fd_set *writefds, int *maxfd) {
    int z;

    for (int i=0; i < peers.len; i++) {
        Peer *peer = ArrayItem(peers, i);

        // If peer socket has a write in progress, add send bytes to write queue.
        SocketCtx *socketctx = SocketCtx_find_by_hostaddr(*socketctxs, peer->hostaddr);
        if (socketctx) {
            BufferAppend(&socketctx->writebuf, msgbytes, msglen);
            continue;
        }

        // Create a new connect socket for this peer.
        int peerfd = socket0(AF_INET, SOCK_STREAM, 0);
        if (peerfd == -1)
            continue;
        int yes=1;
        setsockopt0(peerfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        struct sockaddr_in sa = SockAddrFromHostAddr(peer->hostaddr);
        z = connect0(peerfd, (struct sockaddr *)&sa, sizeof(sa));
        if (z == -1) {
            ShutdownSocket(peerfd);
            continue;
        }

        //printf("Tracker sending msg to %s/%d\n", HostAddr_ipaddress(peer->hostaddr), ntohs(HostAddr_port(peer->hostaddr)));
        // Send bytes to peer.
        Buffer sendbuf = BufferNew(&scratch, msglen);
        BufferAppend(&sendbuf, msgbytes, msglen);
        z = NetSend2(peerfd, &sendbuf, writefds, maxfd);
        if (z == -1) {
            ShutdownSocket(peerfd);
            continue;
        }
        if (z == 0) {
            ShutdownSocket(peerfd);
            continue;
        }
        if (z == 1) {
            SocketCtx newctx;
            newctx.fd = peerfd;
            newctx.hostaddr = peer->hostaddr;
            newctx.readbuf = BufferNew(socketctxs->arena, 1);
            newctx.writebuf = BufferNew(socketctxs->arena, 64);
            BufferAppend(&newctx.writebuf, sendbuf.bs, sendbuf.len);
            newctx.msglen = 0;
            ArrayAppend(socketctxs, &newctx);
        }

        ArenaReset(&scratch);
    }
}

