#include "clib.h"
#include "cnet.h"

#define MSGNO(bs) (*((u8 *)bs))
#define KNOCK 1
#define BYE 2
#define PEER_ONLINE 3
#define PEER_OFFLINE 4

typedef struct {
    u8 msgno;
    String alias;
    String hostname;
    u16 listenport;
} KnockMsg;

typedef struct {
    u8 msgno;
} ByeMsg;

typedef struct {
    u8 msgno;
    String alias;
    String hostname;
    HostAddr fromaddr;
    HostAddr toaddr;
} PeerOnlineMsg;

typedef struct {
    u8 msgno;
    HostAddr fromaddr;
} PeerOfflineMsg;

typedef struct {
    String alias;
    String hostname;
    HostAddr fromaddr;
    HostAddr toaddr;
} Peer;

typedef struct {
    int fd;
    HostAddr fromaddr;
    HostAddr toaddr;
    Buffer readbuf;
    Buffer writebuf;
    u16 msglen;
} SocketCtx;

SocketCtx *SocketCtx_find_by_fd(Array ctxs, int fd);
int SocketCtx_find_by_fd2(Array ctxs, int fd);
SocketCtx *SocketCtx_find_by_toaddr(Array ctxs, HostAddr toaddr);
int SocketCtx_find_by_toaddr2(Array ctxs, HostAddr toaddr);

int Peer_exists(Array peers, HostAddr fromaddr);
void Peer_add_or_replace(Array *peers, Peer *peer);
void Peer_add_or_replace2(Array *peers, String alias, String hostname, HostAddr fromaddr, HostAddr toaddr);
void Peer_remove(Array *peers, HostAddr fromaddr);
void print_peers(Array peers);

int send_msg_to_hostaddr(Arena scratch, char *msgbytes, u16 msglen, HostAddr dest_hostaddr, Array *socketctxs, fd_set *writefds, int *maxfd);
void send_msg_to_peers(Arena scratch, char *msgbytes, u16 msglen, Array peers, Array *socketctxs, fd_set *writefds, int *maxfd);

