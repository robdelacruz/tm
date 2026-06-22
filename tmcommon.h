#include "clib.h"
#include "cnet.h"

#define MSGNO(bs) (*((u8 *)bs))
#define PEER_ONLINE 1
#define PEER_OFFLINE 2
#define KNOCK 3
#define BYE 4
#define CHATTEXT 5

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
    u8 msgno;
    String text;
} ChatTextMsg;

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

typedef struct {
    time_t timestamp;
    String alias;
    String hostname;
    HostAddr fromaddr;
    String text;
} ChatText;

SocketCtx *SocketCtx_find_by_fd(Array ctxs, int fd);
int SocketCtx_find_by_fd2(Array ctxs, int fd);
SocketCtx *SocketCtx_find_by_toaddr(Array ctxs, HostAddr toaddr);
int SocketCtx_find_by_toaddr2(Array ctxs, HostAddr toaddr);

Peer Peer_new(Arena *arena, String alias, String hostname, HostAddr fromaddr, HostAddr toaddr);
void Peer_replace(Peer *peer, String alias, String hostname, HostAddr fromaddr, HostAddr toaddr);
Peer *Peer_find_fromaddr(Array peers, HostAddr fromaddr);
int Peer_find_fromaddr2(Array peers, HostAddr fromaddr);
Peer *Peer_find_alias(Array peers, char *alias);
void print_peers(Array peers);

int send_msg_to_hostaddr(Arena scratch, char *msgbytes, u16 msglen, HostAddr dest_hostaddr, Array *socketctxs, fd_set *writefds, int *maxfd);
void send_msg_to_peers(Arena scratch, char *msgbytes, u16 msglen, Array peers, Array *socketctxs, fd_set *writefds, int *maxfd);

void print_chattexts(Array chattexts);

