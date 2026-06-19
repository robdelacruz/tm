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
SocketCtx *SocketCtx_find_by_toaddr(Array ctxs, HostAddr toaddr) {
    for (int i=0; i < ctxs.len; i++) {
        SocketCtx *ctx = ArrayItem(ctxs, i);
        if (ctx->toaddr == toaddr)
            return ctx;
    }
    return NULL;
}
int SocketCtx_find_by_toaddr2(Array ctxs, HostAddr toaddr) {
    for (int i=0; i < ctxs.len; i++) {
        SocketCtx *ctx = ArrayItem(ctxs, i);
        if (ctx->toaddr == toaddr)
            return i;
    }
    return -1;
}

int Peer_exists(Array peers, HostAddr fromaddr) {
    for (int i=0; i < peers.len; i++) {
        Peer *p = ArrayItem(peers, i);
        if (p->fromaddr == fromaddr)
            return 1;
    }
    return 0;
}

void Peer_add_or_replace(Array *peers, Peer *peer) {
    // Replace if a peer with the same hostaddr exists
    for (int i=0; i < peers->len; i++) {
        Peer *p = ArrayItem(*peers, i);
        if (p->toaddr == peer->toaddr) {
            ArrayReplace(peers, i, peer);
            return;
        }
    }
    ArrayAppend(peers, peer);
}

void Peer_add_or_replace2(Array *peers, String alias, String hostname, HostAddr fromaddr, HostAddr toaddr) {
    Peer peer;
    peer.alias = StringDup(peers->arena, alias);
    peer.hostname = StringDup(peers->arena, hostname);
    peer.fromaddr = fromaddr;
    peer.toaddr = toaddr;

    Peer_add_or_replace(peers, &peer);
}

void Peer_remove(Array *peers, HostAddr fromaddr) {
    for (int i=0; i < peers->len; i++) {
        Peer *p = ArrayItem(*peers, i);
        if (p->fromaddr == fromaddr) {
            ArrayRemove(peers, i);
            return;
        }
    }
}

Peer *Peer_find_fromaddr(Array peers, HostAddr fromaddr) {
    for (int i=0; i < peers.len; i++) {
        Peer *p = ArrayItem(peers, i);
        if (p->fromaddr == fromaddr)
            return p;
    }
    return NULL;
}

void print_peers(Array peers) {
    char *fromaddr_str, *toaddr_str;

    if (peers.len == 0) {
        printf("Peers (0)\n");
        return;
    }

    printf("Peers (%d):\n", peers.len);
    for (int i=0; i < peers.len; i++) {
        Peer *p = ArrayItem(peers, i);

        fromaddr_str = strdup(HostAddr_ipaddress(p->fromaddr));
        toaddr_str = strdup(HostAddr_ipaddress(p->toaddr));
        printf("[%d] %s fromaddr: %s/%d toaddr: %s/%d\n", i+1, CSTR(p->alias), fromaddr_str, ntohs(HostAddr_port(p->fromaddr)), toaddr_str, ntohs(HostAddr_port(p->toaddr)));
        free(fromaddr_str);
        free(toaddr_str);
    }
}

int send_msg_to_hostaddr(Arena scratch, char *msgbytes, u16 msglen, HostAddr toaddr, Array *socketctxs, fd_set *writefds, int *maxfd) {
    int z;

    // If peer socket has a write in progress, add send bytes to write queue.
    SocketCtx *socketctx = SocketCtx_find_by_toaddr(*socketctxs, toaddr);
    if (socketctx) {
        BufferAppend(&socketctx->writebuf, msgbytes, msglen);
        return 1;
    }

    // Create a new connect socket for toaddr.
    int destfd = socket0(AF_INET, SOCK_STREAM, 0);
    if (destfd == -1)
        return -1;
    int yes=1;
    setsockopt0(destfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    printf("Sending msg to %s/%d\n", HostAddr_ipaddress(toaddr), ntohs(HostAddr_port(toaddr)));
    struct sockaddr_in dest_sa = SockAddrFromHostAddr(toaddr);
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
        newctx.fromaddr = 0;
        newctx.toaddr = toaddr;
        newctx.readbuf = BufferNew(socketctxs->arena, 1);
        newctx.writebuf = BufferNew(socketctxs->arena, 64);
        BufferAppend(&newctx.writebuf, sendbuf.bs, sendbuf.len);
        newctx.msglen = 0;
        ArrayAppend(socketctxs, &newctx);
    }

    ShutdownSocket(destfd);
    return 0;
}

void send_msg_to_peers(Arena scratch, char *msgbytes, u16 msglen, Array peers, Array *socketctxs, fd_set *writefds, int *maxfd) {
    for (int i=0; i < peers.len; i++) {
        Peer *p = ArrayItem(peers, i);
        send_msg_to_hostaddr(scratch, msgbytes, msglen, p->toaddr, socketctxs, writefds, maxfd);
    }
}

