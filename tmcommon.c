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
SocketCtx *SocketCtx_find_by_hostaddr(Array ctxs, HostAddr hostaddr) {
    for (int i=0; i < ctxs.len; i++) {
        SocketCtx *ctx = ArrayItem(ctxs, i);
        if (ctx->hostaddr == hostaddr)
            return ctx;
    }
    return NULL;
}
int SocketCtx_find_by_hostaddr2(Array ctxs, HostAddr hostaddr) {
    for (int i=0; i < ctxs.len; i++) {
        SocketCtx *ctx = ArrayItem(ctxs, i);
        if (ctx->hostaddr == hostaddr)
            return i;
    }
    return -1;
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

void print_peers(Array peers) {
    printf("Peers:\n");
    for (int i=0; i < peers.len; i++) {
        Peer *p = ArrayItem(peers, i);
        printf("[%d] %s %s/%d\n", i+1, CSTR(p->alias), HostAddr_ipaddress(p->hostaddr), ntohs(HostAddr_port(p->hostaddr)));
    }
}


