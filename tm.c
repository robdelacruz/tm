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
//#define TRACKER_HOST "google.com"
//#define TRACKER_PORT "8001"
#define HOST_PORT 9000

int send_msg_to_tracker(Buffer *sendbuf, struct timeval *timeout);
void* THREAD_wait_for_tcp_messages(void *data);
void handle_msg(Arena scratch, int fd, HostAddr hostaddr, char *msgbytes, u16 msglen, Array *socketctxs, fd_set *writefds, int *maxfd);
int send_msg_to_hostaddr(Arena scratch, char *msgbytes, u16 msglen, HostAddr dest_hostaddr, Array *socketctxs, fd_set *writefds, int *maxfd);

u16 GHostPort = HOST_PORT;
char GHostPortStr[10];
String GAlias;
String GHostname;
Array GPeers;

int main(int argc, char *argv[]) {
    int z;
    Arena arena = ArenaNew(64*1024);
    Arena scratch = ArenaNew(1024);

    if (getlogin() != NULL)
        GAlias = StringNew(&arena, getlogin());
    else
        GAlias = StringNew(&arena, "noname");
    char hostname[HOST_NAME_MAX];
    if (gethostname(hostname, sizeof(hostname)) != -1) {
        hostname[HOST_NAME_MAX-1] = 0;
        GHostname = StringNew(&arena, hostname);
    } else {
        GHostname = StringNew(&arena, "nohostname");
    }

    if (argc > 1) {
        int n = atoi(argv[1]);
        if (n > 0) {
            GHostPort += n;

            char ns[12];
            snprintf(ns, sizeof(ns), "%d", n);
            StringAppend(&GAlias, ns);
        }
    }
    snprintf(GHostPortStr, sizeof(GHostPortStr), "%d", GHostPort);
    printf("Hello %s/%s port %d\n", CSTR(GAlias), CSTR(GHostname), GHostPort);

    GPeers = ArrayNew(&arena, 64, sizeof(Peer));

    pthread_t thread_wait_tcp;
    pthread_create(&thread_wait_tcp, NULL, THREAD_wait_for_tcp_messages, NULL);

    Buffer sendbuf = BufferNew(&scratch, 128);
    NetPackLen(&sendbuf, "%b%s%s%w", KNOCK, CSTR(GAlias), CSTR(GHostname), GHostPort);
    struct timeval timeout = {2, 0};
    z = send_msg_to_tracker(&sendbuf, &timeout);
    if (z == -1)
        return 1;

    pthread_join(thread_wait_tcp, NULL);
    return 0;
}

int send_msg_to_tracker(Buffer *sendbuf, struct timeval *timeout) {
    int z;
    int trackerfd;
    struct sockaddr_in sa;

    trackerfd = OpenTcpConnectSocket(0, TRACKER_HOST, TRACKER_PORT, timeout);
    if (trackerfd == -1) {
        fprintf(stderr, "Error opening tracker socket\n");
        return -1;
    }

    z = NetSend(trackerfd, sendbuf);
    if (z == -1) {
        fprintf(stderr, "Error sending message to tracker (%s)\n", strerror(errno));
        ShutdownSocket(trackerfd);
        return -1;
    }
    if (z == 0) {
        ShutdownSocket(trackerfd);
        return 0;
    }

    fd_set writefds;
    FD_ZERO(&writefds);
    FD_SET(trackerfd, &writefds);

    while (1) {
        z = select(trackerfd+1, NULL, &writefds, NULL, timeout);
        if (z == 0) {
            fprintf(stderr, "Timeout sending message to tracker (%s)\n", strerror(errno));
            ShutdownSocket(trackerfd);
            return -1;
        }
        if (z == -1 && errno == EINTR)
            continue;
        if (z == -1) {
            fprintf(stderr, "Error sending message to tracker (%s)\n", strerror(errno));
            ShutdownSocket(trackerfd);
            return -1;
        }
        if (FD_ISSET(trackerfd, &writefds)) {
            z = NetSend(trackerfd, sendbuf);
            if (z == -1) {
                fprintf(stderr, "Error sending message to tracker (%s)\n", strerror(errno));
                ShutdownSocket(trackerfd);
                return -1;
            }
            if (z == 0)
                break;
        }
    }

    ShutdownSocket(trackerfd);
    return 0;
}

void* THREAD_wait_for_tcp_messages(void *data) {
    int z;
    int listenfd;
    struct sockaddr_in sa;

    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd == -1) {
        fprintf(stderr, "socket(): %s\n", strerror(errno));
        return NULL;
    }
    int yes=1;
    z = setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (z == -1) {
        fprintf(stderr, "setsockopt(): %s\n", strerror(errno));
        return NULL;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(GHostPort);
    sa.sin_addr.s_addr = INADDR_ANY;
    z = bind(listenfd, (struct sockaddr *) &sa, sizeof(sa));
    if (z == -1) {
        fprintf(stderr, "bind(): %s\n", strerror(errno));
        return NULL;
    }

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

    printf("Listening for messages on port %d...\n", GHostPort);
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
                    // New socket connection
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
                    ArrayAppend(&socketctxs, &socketctx);

                    FD_SET(socketfd, &readfds);
                    if (socketfd > maxfd)
                        maxfd = socketfd;

                    printf("New socketfd: %d\n", socketfd);
                } else {
                    // Received bytes from socket
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
                            ArenaReset(&tscratch);
                            socketctxs = ArrayNew(&tscratch, 64, sizeof(SocketCtx));
                        }
                        //printf("Closed socketfd %d\n", socketfd);
                    }
                }
            }
        }
    }

    ShutdownSocket(listenfd);
}

void handle_msg(Arena scratch, int fd, HostAddr hostaddr, char *msgbytes, u16 msglen, Array *socketctxs, fd_set *writefds, int *maxfd) {
    u8 msgno = MSGNO(msgbytes);
    if (msgno == PING) {
        String alias = StringNew0(&scratch);
        String hostname = StringNew0(&scratch);
        HostAddr hostaddr_from;
        String text = StringNew0(&scratch);
        NetUnpack(msgbytes, msglen, "%b%s%s%L%s", &msgno, &alias, &hostname, &hostaddr_from, &text);
        printf("** PING alias: '%s' hostname: '%s' hostaddr_from: %s/%d text: '%s' **\n", CSTR(alias), CSTR(hostname), HostAddr_ipaddress(hostaddr_from), ntohs(HostAddr_port(hostaddr_from)), CSTR(text));

        Buffer sendbuf = BufferNew(&scratch, 1024);

        if (StringEquals(text, "knock")) {
            // Tracker forwarded 'knock', source address is in hostaddr_from 

            // Add new peer from hostaddr_from 
            Peer peer;
            peer.hostaddr = hostaddr_from;
            peer.alias = StringDup(GPeers.arena, alias);
            peer.hostname = StringDup(GPeers.arena, hostname);
            Peer_add_or_replace(&GPeers, peer);
            print_peers(GPeers);

            // Send back 'hello' to new peer that sent knock
            NetPackLen(&sendbuf, "%b%s%s%L%s", PING, CSTR(GAlias), CSTR(GHostname), 0, "hello");
            send_msg_to_hostaddr(scratch, sendbuf.bs, sendbuf.len, hostaddr_from, socketctxs, writefds, maxfd);
        } else if (StringEquals(text, "hello")) {
            // Peer sent 'hello' directly
            Peer peer;
            peer.hostaddr = hostaddr;
            peer.alias = StringDup(GPeers.arena, alias);
            peer.hostname = StringDup(GPeers.arena, hostname);
            Peer_add_or_replace(&GPeers, peer);
            print_peers(GPeers);
        } else if (StringEquals(text, "bye")) {
            // Tracker forwarded 'bye', source addressed is in hostaddr_from
            Peer_remove(&GPeers, hostaddr_from);
            print_peers(GPeers);
        }
    }
}

int send_msg_to_hostaddr(Arena scratch, char *msgbytes, u16 msglen, HostAddr dest_hostaddr, Array *socketctxs, fd_set *writefds, int *maxfd) {
    int z;

    // If peer socket has a write in progress, add send bytes to write queue.
    SocketCtx *socketctx = SocketCtx_find_by_hostaddr(*socketctxs, dest_hostaddr);
    if (socketctx) {
        BufferAppend(&socketctx->writebuf, msgbytes, msglen);
        return 1;
    }

    // Create a new connect socket for dest_hostaddr.
    int destfd = socket0(AF_INET, SOCK_STREAM, 0);
    if (destfd == -1)
        return -1;
    int yes=1;
    setsockopt0(destfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    printf("Sending msg to %s/%d\n", HostAddr_ipaddress(dest_hostaddr), ntohs(HostAddr_port(dest_hostaddr)));
    struct sockaddr_in dest_sa = SockAddrFromHostAddr(dest_hostaddr);
    z = connect0(destfd, (struct sockaddr *)&dest_sa, sizeof(dest_sa));
    if (z == -1) {
        ShutdownSocket(destfd);
        return -1;
    }

    // Send bytes
    Buffer sendbuf = BufferNew(&scratch, msglen);
    BufferAppend(&sendbuf, msgbytes, msglen);
    z = NetSend2(destfd, &sendbuf, writefds, maxfd);
    if (z == -1) {
        ShutdownSocket(destfd);
        return -1;
    }
    if (z == 0) {
        ShutdownSocket(destfd);
        return 0;
    }
    if (z == 1) {
        SocketCtx newctx;
        newctx.fd = destfd;
        newctx.hostaddr = dest_hostaddr;
        newctx.readbuf = BufferNew(socketctxs->arena, 1);
        newctx.writebuf = BufferNew(socketctxs->arena, 64);
        BufferAppend(&newctx.writebuf, sendbuf.bs, sendbuf.len);
        newctx.msglen = 0;
        ArrayAppend(socketctxs, &newctx);
    }

    ShutdownSocket(destfd);
    return 0;
}


