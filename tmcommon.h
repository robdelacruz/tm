#include "clib.h"
#include "cnet.h"

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
} SocketCtx;

SocketCtx *SocketCtx_find_by_fd(Array ctxs, int fd);
int SocketCtx_find_by_fd2(Array ctxs, int fd);
SocketCtx *SocketCtx_find_by_hostaddr(Array ctxs, HostAddr hostaddr);
int SocketCtx_find_by_hostaddr2(Array ctxs, HostAddr hostaddr);

void Peer_add_or_replace(Array *peers, Peer peer);
void Peer_remove(Array *peers, HostAddr hostaddr);
void print_peers(Array peers);


