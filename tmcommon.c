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
            GtkListBox_replace(GUI.peerslistbox, i, CSTR(p->alias));
            return;
        }
    }
    ArrayAppend(peers, peer);
    GtkListBox_append(GUI.peerslistbox, CSTR(p->alias));
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
            GtkListBox_remove(GUI.peerslistbox, i);
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

Peer *Peer_find_alias(Array peers, char *alias) {
    for (int i=0; i < peers.len; i++) {
        Peer *p = ArrayItem(peers, i);
        if (StringEquals(p->alias, alias))
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

void print_chattexts(Array chattexts) {
    if (chattexts.len == 0) {
        printf("Chat Texts (0)\n");
        return;
    }

    printf("Chat Texts (%d):\n", chattexts.len);
    for (int i=0; i < chattexts.len; i++) {
        ChatText *p = ArrayItem(chattexts, i);
        printf("[%d] From %s/%s: %s\n", i+1, CSTR(p->alias), CSTR(p->hostname), CSTR(p->text));
    }
}

GtkWidget *create_label(char *caption) {
    GtkWidget *lbl = gtk_label_new(caption);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_widget_set_valign(lbl, GTK_ALIGN_END);
    return lbl;
}

int GtkListBox_numrows(GtkWidget *lb) {
    for (int i=0; ; i++) {
        GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(lb), i);
        if (row == NULL)
            return i;
    }
    assert(FALSE);
    return 0;
}
void GtkListBox_append(GtkWidget *lb, char *text) {
    GtkWidget *lbl = create_label(text);
    gtk_container_add(GTK_CONTAINER(lb), lbl);
}
void GtkListBox_replace(GtkWidget *lb, int index, char *text) {
    GtkWidget *row = (GtkWidget *) gtk_list_box_get_row_at_index(GTK_LIST_BOX(lb), index);
    if (row == NULL)
        return;
    gtk_container_remove(GTK_CONTAINER(lb), row);
    GtkWidget *lbl = create_label(text);
    gtk_list_box_insert(GTK_LIST_BOX(lb), lbl, index);
}
void GtkListBox_remove(GtkListBox *lb, int index) {
    GtkWidget *row = (GtkWidget *) gtk_list_box_get_row_at_index(GTK_LIST_BOX(lb), index);
    if (row)
        gtk_container_remove(GTK_CONTAINER(lb), row);
}

