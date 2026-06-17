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
#include <ctype.h>

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
#define SEND_PORT 8100
#define LISTEN_PORT 8200

void sigint(int sig);
void parse_args(int argc, char **argv);
void send_bye(Arena scratch);
int execute_command(Arena scratch, char *cmd);
Array tokenize_command(Arena *arena, char *cmd);
void array_join_strings(Array strs, int istart, int iend, String *outstr);

void* THREAD_wait_for_tcp_messages(void *data);
void handle_msg(Arena scratch, int fd, HostAddr fromaddr, char *msgbytes, u16 msglen, Array *socketctxs, fd_set *writefds, int *maxfd);

String GTrackerHost;
u16 GTrackerPort = TRACKER_PORT;
u16 GSendPort = SEND_PORT;
u16 GListenPort = LISTEN_PORT;
String GAlias;
String GHostname;

Array GPeers;
int GTrackerFD=0;
HostAddr GTrackerAddr=0;

int GUnblockSelect_writefd=0;

int main(int argc, char *argv[]) {
    int z;
    Arena arena = ArenaNew(64*1024);
    Arena scratch = ArenaNew(1024);

    signal(SIGINT, sigint);

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
    GTrackerHost = StringNew(&arena, TRACKER_HOST);
    GTrackerPort = TRACKER_PORT;

    parse_args(argc, argv);

    printf("TinyMsg listenport: %d sendport: %d user: %s/%s\n", GListenPort, GSendPort, CSTR(GAlias), CSTR(GHostname));
    printf("Connected to tracker %s/%d\n", CSTR(GTrackerHost), GTrackerPort);

    struct sockaddr_in tracker_sa;
    if (GetIPV4Address(CSTR(GTrackerHost), GTrackerPort, &tracker_sa) == -1) {
        printf("Can't resolve tracker's (%s/%d) sockaddr\n", CSTR(GTrackerHost), GTrackerPort);
        exit(1);
    }
    GTrackerAddr = HostAddrFromSockAddr(&tracker_sa);

    GPeers = ArrayNew(&arena, 64, sizeof(Peer));

    struct timeval timeout = {2, 0};
    GTrackerFD = OpenTcpConnectSocket(GSendPort, CSTR(GTrackerHost), GTrackerPort, &timeout);
    if (GTrackerFD == -1) {
        printf("Can't connect to tracker %s/%d\n", CSTR(GTrackerHost), GTrackerPort);
        exit(1);
    }

    pthread_t thread_wait_tcp;
    pthread_create(&thread_wait_tcp, NULL, THREAD_wait_for_tcp_messages, NULL);

    Buffer sendbuf = BufferNew(&scratch, 128);
    NetPackLen(&sendbuf, "%b%s%s%w", KNOCK, CSTR(GAlias), CSTR(GHostname), GListenPort);
    timeout = (struct timeval) {2, 0};
    z = NetSend_wait_until_complete(GTrackerFD, &sendbuf, &timeout);
    if (z == -1) {
        printf("Error sending knock to tracker %s/%d\n", CSTR(GTrackerHost), GTrackerPort);
        return 1;
    }

    while (1) {
        char inputbuf[64];

        printf("> ");
        fgets(inputbuf, sizeof(inputbuf), stdin);
        inputbuf[strlen(inputbuf)-1] = 0; // remove trailing \n
        if (execute_command(scratch, inputbuf) == 1)
            break;
    }

    // Break out of select() loop in THREAD_wait_for_tcp_messages().
    char writebuf[] = "1";
    z = write(GUnblockSelect_writefd, writebuf, sizeof(writebuf));
    if (z == -1)
        fprintf(stderr, "write(): %s\n", strerror(errno));
    assert(z > 0);

    send_bye(scratch);
    ShutdownSocket(GTrackerFD);

    pthread_join(thread_wait_tcp, NULL);

    printf("Bye.\n");
    return 0;
}

void sigint(int sig) {
    Arena scratch = ArenaNew(256);
    send_bye(scratch);
    printf("Ctrl-C Bye.\n");
    exit(0);
}

void send_bye(Arena scratch) {
    Buffer sendbuf = BufferNew(&scratch, 50);
    NetPackLen(&sendbuf, "%b", BYE);
    struct timeval timeout = {2, 0};
    NetSend_wait_until_complete(GTrackerFD, &sendbuf, &timeout);
}

enum ParseArgs {SW_NONE, SW_LISTENPORT, SW_SENDPORT, SW_TRACKERHOST, SW_TRACKERPORT, SW_ALIAS, SW_HOSTNAME};
void parse_args(int argc, char **argv) {
    enum ParseArgs state = SW_NONE;

    for (int i=0; i < argc; i++) {
        char *arg = argv[i];
        if (state == SW_NONE) {
            if (CSTR_EQUALS(arg, "-listenport"))
                state = SW_LISTENPORT;
            else if (CSTR_EQUALS(arg, "-sendport"))
                state = SW_SENDPORT;
            else if (CSTR_EQUALS(arg, "-trackerhost"))
                state = SW_TRACKERHOST;
            else if (CSTR_EQUALS(arg, "-trackerport"))
                state = SW_TRACKERPORT;
            else if (CSTR_EQUALS(arg, "-alias"))
                state = SW_ALIAS;
            else if (CSTR_EQUALS(arg, "-hostname"))
                state = SW_HOSTNAME;
            continue;
        }

        if (state == SW_LISTENPORT)
            GListenPort = atoi(arg);
        else if (state == SW_SENDPORT)
            GSendPort = atoi(arg);
        else if (state == SW_TRACKERPORT)
            GTrackerPort = atoi(arg);
        else if (state == SW_TRACKERHOST)
            StringAssign(&GTrackerHost, arg);
        else if (state == SW_ALIAS)
            StringAssign(&GAlias, arg);
        else if (state == SW_HOSTNAME)
            StringAssign(&GHostname, arg);
        state = SW_NONE;
    }
}

enum ExecState {EXEC_START, EXEC_SEND, EXEC_SENDALIAS};
int execute_command(Arena scratch, char *cmd) {
    Array tokens = tokenize_command(&scratch, cmd);

//    printf("execute_command() tokens (%d):\n", tokens.len);
//    for (int i=0; i < tokens.len; i++) {
//        String *tok = ArrayItem(tokens, i);
//        printf("[%d] %s\n", i, CSTR(*tok));
//    }

    if (tokens.len == 0)
        return 0;

    String sendalias;
    String chattext = StringNew0(&scratch);

    enum ExecState state = EXEC_START;
    for (int i=0; i < tokens.len; i++) {
        String *ptoken = ArrayItem(tokens, i);
        String token = *ptoken;

        if (state == EXEC_START) {
            if (StringEquals(token, "quit") || StringEquals(token, "bye")) {
                return 1;
            } else if (StringEquals(token, "knock")) {
                return 0;
            } else if (StringEquals(token, "peers")) {
                print_peers(GPeers);
                return 0;
            } else if (StringEquals(token, "chats")) {
                return 0;
            } else if (StringEquals(token, "send")) {
                state = EXEC_SEND;
            }
        } else if (state == EXEC_SEND) {
            sendalias = token;
            state = EXEC_SENDALIAS;
        } else if (state == EXEC_SENDALIAS) {
            array_join_strings(tokens, i, tokens.len-1, &chattext);
            if (chattext.len == 0)
                return 0;

            //todo: send chattext to sendalias
            return 0;
        }
    }

    return 0;
}

enum TokenizeState {TOKENIZE_START, TOKENIZE_SPACE, TOKENIZE_CHAR, TOKENIZE_EOF};
Array tokenize_command(Arena *arena, char *cmd) {
    Array tokens = ArrayNew(arena, 16, sizeof(String));
    enum TokenizeState state = TOKENIZE_SPACE;
    String token = StringNew0(arena);

    int cmdlen = strlen(cmd);
    for (int i=0; i <= cmdlen; i++) {
        char ch = cmd[i];

        if (state == TOKENIZE_START) {
            if (isalnum(ch)) {
                StringAppendChar(&token, ch);
                state = TOKENIZE_CHAR;
            } else if (isspace(ch)) {
                state = TOKENIZE_SPACE;
            } else if (ch == '\0') {
                state = TOKENIZE_EOF;
            }
        } else if (state == TOKENIZE_SPACE) {
            if (isalnum(ch)) {
                StringAppendChar(&token, ch);
                state = TOKENIZE_CHAR;
            } else if (isspace(ch)) {
                state = TOKENIZE_SPACE;
            } else if (ch == '\0') {
                state = TOKENIZE_EOF;
            }
        } else if (state == TOKENIZE_CHAR) {
            if (isalnum(ch)) {
                StringAppendChar(&token, ch);
                state = TOKENIZE_CHAR;
            } else if (isspace(ch)) {
                ArrayAppend(&tokens, &token);
                token = StringNew0(arena);
                state = TOKENIZE_SPACE;
            } else if (ch == '\0') {
                ArrayAppend(&tokens, &token);
                state = TOKENIZE_EOF;
            }
        }
    }

    return tokens;
}

void array_join_strings(Array strs, int istart, int iend, String *outstr) {
    StringAssign(outstr, "");

    if (iend > strs.len-1)
        iend = strs.len-1;

    for (int i=istart; i <= iend; i++) {
        String *s = ArrayItem(strs, i);
        StringAppend(outstr, CSTRP(s));
        if (i < iend)
            StringAppend(outstr, " ");
    }
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
    sa.sin_port = htons(GListenPort);
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

    // unblockselect_pipefds[0]: read
    // unblockselect_pipefds[1]: write
    int pipefds[2];
    z = pipe(pipefds);
    assert(z == 0);
    GUnblockSelect_writefd = pipefds[1];
    FD_SET(pipefds[0], &readfds);
    if (pipefds[0] > maxfd)
        maxfd = pipefds[0];

    Arena tscratch = ArenaNew(1024*1024);
    Array socketctxs = ArrayNew(&tscratch, 64, sizeof(SocketCtx));

    //printf("Listening for messages on port %d...\n", GListenPort);
    int is_running = 1;
    while (is_running) {
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
                // This provides a way for the program to break out of the select() loop.
                // write to GUnblockSelect_writefd to break out of loop.
                if (i == pipefds[0]) {
                    char buf[10];
                    read(i, buf, sizeof(buf));
                    is_running = 0;
                    break;
                }

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
                    socketctx.fromaddr = HostAddrFromSockAddr((struct sockaddr_in *)&ss);
                    socketctx.toaddr = 0;
                    socketctx.readbuf = BufferNew(&tscratch, 64);
                    socketctx.writebuf = BufferNew(&tscratch, 64);
                    socketctx.msglen = 0;
                    ArrayAppend(&socketctxs, &socketctx);

                    FD_SET(socketfd, &readfds);
                    if (socketfd > maxfd)
                        maxfd = socketfd;

                    //printf("New socketfd: %d\n", socketfd);
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
    ArenaFree(&tscratch);
    return NULL;
}

void handle_msg(Arena scratch, int fd, HostAddr fromaddr, char *msgbytes, u16 msglen, Array *socketctxs, fd_set *writefds, int *maxfd) {
    u8 msgno = MSGNO(msgbytes);

    if (msgno == PEER_ONLINE) {
        // Should only come from tracker
        if (!HostAddr_ipaddr_equals(fromaddr, GTrackerAddr))
            return;

        String alias = StringNew0(&scratch);
        String hostname = StringNew0(&scratch);
        HostAddr peer_fromaddr;
        HostAddr peer_toaddr;
        NetUnpack(msgbytes, msglen, "%b%s%s%L%L", &msgno, &alias, &hostname, &peer_fromaddr, &peer_toaddr);

        printf("** PEER_ONLINE alias: %s' hostname: '%s' fromaddr: %s/%d toaddr: %s/%d **\n", CSTR(alias), CSTR(hostname), HostAddr_ipaddress(peer_fromaddr), ntohs(HostAddr_port(peer_fromaddr)), HostAddr_ipaddress(peer_toaddr), ntohs(HostAddr_port(peer_toaddr)));

        Peer_add_or_replace2(&GPeers, alias, hostname, peer_fromaddr, peer_toaddr);
        print_peers(GPeers);
    } else if (msgno == PEER_OFFLINE) {
        // Should only come from tracker
        if (!HostAddr_ipaddr_equals(fromaddr, GTrackerAddr))
            return;

        HostAddr peer_fromaddr;
        NetUnpack(msgbytes, msglen, "%b%L", &msgno, &peer_fromaddr);

        printf("** PEER_OFFLINE fromaddr: %s/%d **\n", HostAddr_ipaddress(peer_fromaddr), ntohs(HostAddr_port(peer_fromaddr)));

        Peer_remove(&GPeers, peer_fromaddr);
        print_peers(GPeers);
    }
}

