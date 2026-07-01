#include "clib.h"
#include "cnet.h"

#define MSGNO(bs) (*((u8 *)bs))
#define PEER_ONLINE 1
#define PEER_OFFLINE 2
#define KNOCK 3
#define BYE 4
#define CHATTEXT 5

typedef int TMHandle;

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
    TMHandle hpeer;
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
    HostAddr fromaddr;
    HostAddr toaddr;
    String text;
} ChatText;

SocketCtx *SocketCtx_find_by_fd(Array ctxs, int fd);
int SocketCtx_find_by_fd2(Array ctxs, int fd);
SocketCtx *SocketCtx_find_by_toaddr(Array ctxs, HostAddr toaddr);
int SocketCtx_find_by_toaddr2(Array ctxs, HostAddr toaddr);

void init_tmdata(Arena *arena, Arena scratch);
Array get_peers_array();
void print_peers();
TMHandle create_peer(char *alias, char *hostname, HostAddr fromaddr, HostAddr toaddr);
void destroy_peer(TMHandle hpeer);
TMHandle find_peer_fromaddr(HostAddr fromaddr);
TMHandle find_peer_alias(char *alias);
int get_peer_data(TMHandle hpeer, String *alias, String *hostname, HostAddr *fromaddr, HostAddr *toaddr);

int send_msg_to_hostaddr(Arena scratch, char *msgbytes, u16 msglen, HostAddr dest_hostaddr, Array *socketctxs, fd_set *writefds, int *maxfd);
void send_msg_to_peers(Arena scratch, char *msgbytes, u16 msglen, Array *socketctxs, fd_set *writefds, int *maxfd);

void print_chattexts(Arena scratch, Array chattexts);

