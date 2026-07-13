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
#include <stdarg.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include <gtk/gtk.h>
#include "clib.h"
#include "cnet.h"
#include "tmcommon.h"
#include "uicommon.h"

#define TRACKER_HOST "127.0.0.1"
#define TRACKER_PORT 9000
#define SEND_PORT 8100
#define LISTEN_PORT 8200

typedef struct {
    GtkWidget *win;
    GtkWidget *peerslb;
} UIMainWin;

typedef struct {
    TMHandle hpeer;
    GtkWidget *win;
    GtkWidget *msghistorylb;
    GtkWidget *sendtext;
    GtkWidget *scrolltext;
    GtkWidget *statusbar;
    guint statusbar_ctxid;
    GtkWidget *sendbtn;
} UIChatWin;

typedef struct {
    gpointer param[5];
} DataParams;

void sigint(int sig);
void parse_args(int argc, char **argv);
int execute_command(Arena scratch, char *cmd);
void run_shell(Arena scratch);

void* THREAD_wait_for_tcp_messages(void *data);
void handle_msg(Arena scratch, int fd, HostAddr fromaddr, char *msgbytes, u16 msglen, Array *socketctxs, fd_set *writefds, int *maxfd);

int SendMsg(Arena scratch, int destfd, struct timeval *timeout, char *fmt, ...);
int ConnectAndSendMsg(Arena scratch, int bindport, SockAddrIPV4 *sa, struct timeval *timeout, char *fmt, ...);

void open_mainwin(Arena scratch);
void open_chatwin(Arena scratch, TMHandle hpeer);

UIChatWin *find_chatwin_from_peer(TMHandle hpeer);
int find_chatwin_from_peer2(TMHandle hpeer);
void refresh_msghistory(Arena scratch, GtkWidget *msglb, Array chattexts, char *peer_alias, char *peer_hostname, HostAddr peer_fromaddr, HostAddr peer_toaddr);

static void CB_select_peer(GtkWidget *w, GtkListBoxRow *row, gpointer data);
static gboolean CB_chatwin_delete(GtkWidget *w, GdkEvent *e, gpointer data);
static void CB_chatwin_destroy(GtkWidget *w, gpointer data);
static void CB_chatwin_send(GtkWidget *w, gpointer data);

static gboolean IDLE_peer_online(gpointer data);
static gboolean IDLE_peer_offline(gpointer data);
static gboolean IDLE_open_chatwin(gpointer data);

String GTrackerHost;
u16 GTrackerPort = TRACKER_PORT;
u16 GSendPort = SEND_PORT;
u16 GListenPort = LISTEN_PORT;
String GAlias;
String GHostname;
HostAddr GTrackerAddr=0;
SockAddrIPV4 GTrackerSockAddr;

int GUnblockSelect_writefd=0;

UIMainWin GMainWin = {0};
Array GChatTexts;
Array GChatWins;

Arena GScratch1;

int main(int argc, char *argv[]) {
    int z;
    Arena arena_globals = ArenaNew(4*1024);
    GScratch1 = ArenaNew(4*1024);

    gtk_init(&argc, &argv);

    signal(SIGINT, sigint);

    if (getlogin() != NULL)
        GAlias = StringNew(&arena_globals, getlogin());
    else
        GAlias = StringNew(&arena_globals, "noname");
    char hostname[HOST_NAME_MAX];
    if (gethostname(hostname, sizeof(hostname)) != -1) {
        hostname[HOST_NAME_MAX-1] = 0;
        GHostname = StringNew(&arena_globals, hostname);
    } else {
        GHostname = StringNew(&arena_globals, "nohostname");
    }
    GTrackerHost = StringNew(&arena_globals, TRACKER_HOST);
    GTrackerPort = TRACKER_PORT;

    parse_args(argc, argv);

    printf("TinyMsg listenport: %d sendport: %d user: %s/%s\n", GListenPort, GSendPort, CSTR(GAlias), CSTR(GHostname));
    printf("Using tracker %s/%d\n", CSTR(GTrackerHost), GTrackerPort);

    if (GetIPV4Address(CSTR(GTrackerHost), GTrackerPort, &GTrackerSockAddr) == -1) {
        printf("Can't resolve tracker's (%s/%d) sockaddr\n", CSTR(GTrackerHost), GTrackerPort);
        exit(1);
    }
    GTrackerAddr = HostAddrFromSockAddr(&GTrackerSockAddr);

    // Chat texts should probably be in a sqlite3 database rather than in memory.
    Arena arena_chats = ArenaNew(64*1024);
    GChatTexts = ArrayNew(&arena_chats, 64, sizeof(ChatText));

    Arena arena_chatwins = ArenaNew(4*1024);
    GChatWins = ArrayNew(&arena_chatwins, 64, sizeof(UIChatWin));

    pthread_t thread_wait_tcp;
    pthread_create(&thread_wait_tcp, NULL, THREAD_wait_for_tcp_messages, NULL);
    //GThread *thread_wait_tcp = g_thread_new("wait_for_tcp_messages", THREAD_wait_for_tcp_messages, NULL);

    struct timeval timeout = {2, 0};
    z = ConnectAndSendMsg(GScratch1, GSendPort, &GTrackerSockAddr, &timeout, "%b%s%s%w", KNOCK, CSTR(GAlias), CSTR(GHostname), GListenPort);
    if (z == -1) {
        printf("Error sending knock to tracker %s/%d\n", CSTR(GTrackerHost), GTrackerPort);
        return 1;
    }

    open_mainwin(GScratch1);
    gtk_main();

    // Break out of select() loop in THREAD_wait_for_tcp_messages().
    char writebuf[] = "1";
    z = write(GUnblockSelect_writefd, writebuf, sizeof(writebuf));
    if (z == -1)
        fprintf(stderr, "write(): %s\n", strerror(errno));
    assert(z > 0);

    ConnectAndSendMsg(GScratch1, GSendPort, &GTrackerSockAddr, &timeout, "%b", BYE);

    pthread_join(thread_wait_tcp, NULL);
    //g_thread_join(thread_wait_tcp);

    printf("Bye.\n");
    return 0;
}

void run_shell(Arena scratch) {
    while (1) {
        char inputbuf[64];

        printf("> ");
        fgets(inputbuf, sizeof(inputbuf), stdin);
        inputbuf[strlen(inputbuf)-1] = 0; // remove trailing \n
        if (execute_command(scratch, inputbuf) == 1)
            break;
    }
}

void sigint(int sig) {
    Arena scratch = GScratch1;

    struct timeval timeout = {2, 0};
    ConnectAndSendMsg(scratch, GSendPort, &GTrackerSockAddr, &timeout, "%b", BYE);
    printf("Ctrl-C Bye.\n");
    exit(0);
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

enum ExecState {EXEC_START, EXEC_SEND, EXEC_SENDALIAS};
int execute_command(Arena scratch, char *cmd) {
    Array tokens = tokenize_command(&scratch, cmd);

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
                print_peers();
                return 0;
            } else if (StringEquals(token, "chats")) {
                print_chattexts(scratch, GChatTexts);
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

            // Send chattext to sendalias
            TMHandle hpeer = find_peer_alias(CSTR(sendalias));
            if (hpeer == -1) {
                printf("Alias %s is not online.\n", CSTR(sendalias));
                return 0;
            }

            HostAddr toaddr=0;
            get_peer_data(hpeer, NULL, NULL, NULL, &toaddr);
            SockAddrIPV4 to_sa = SockAddrFromHostAddr(toaddr);

            struct timeval timeout = {2,0};
            int z = ConnectAndSendMsg(scratch, GSendPort, &to_sa, &timeout, "%b%s", CHATTEXT, CSTR(chattext));
            if (z == -1)
                printf("Error sending to peer %s (%s/%d)\n", CSTR(sendalias), HostAddr_ipaddress(toaddr), ntohs(HostAddr_port(toaddr)));
            return 0;
        }
    }

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

    // PEER_ONLINE
    if (msgno == PEER_ONLINE) {
        // Should only come from tracker
        if (!HostAddr_ipaddr_equals(fromaddr, GTrackerAddr))
            return;

        String alias = StringNew0(&scratch);
        String hostname = StringNew0(&scratch);
        HostAddr peer_fromaddr;
        HostAddr peer_toaddr;
        NetUnpack(msgbytes, msglen, "%b%s%s%L%L", &msgno, &alias, &hostname, &peer_fromaddr, &peer_toaddr);

        printf("** PEER_ONLINE %s/%s fromaddr: %s/%d toaddr: %s/%d **\n", CSTR(alias), CSTR(hostname), HostAddr_ipaddress(peer_fromaddr), ntohs(HostAddr_port(peer_fromaddr)), HostAddr_ipaddress(peer_toaddr), ntohs(HostAddr_port(peer_toaddr)));

        TMHandle hpeer = create_peer(CSTR(alias), CSTR(hostname), peer_fromaddr, peer_toaddr);
        g_idle_add(IDLE_peer_online, GINT_TO_POINTER(hpeer));
        print_peers();

    // PEER_OFFLINE
    } else if (msgno == PEER_OFFLINE) {
        // Should only come from tracker
        if (!HostAddr_ipaddr_equals(fromaddr, GTrackerAddr))
            return;

        HostAddr peer_fromaddr;
        NetUnpack(msgbytes, msglen, "%b%L", &msgno, &peer_fromaddr);

        printf("** PEER_OFFLINE fromaddr: %s/%d **\n", HostAddr_ipaddress(peer_fromaddr), ntohs(HostAddr_port(peer_fromaddr)));

        TMHandle hpeer = find_peer_fromaddr(peer_fromaddr);
        if (hpeer != -1) {
            g_idle_add(IDLE_peer_offline, GINT_TO_POINTER(hpeer));
        }
        print_peers();

    // CHATTEXT
    } else if (msgno == CHATTEXT) {
        String text = StringNew0(&scratch);
        NetUnpack(msgbytes, msglen, "%b%s", &msgno, &text);

        TMHandle hpeer = find_peer_fromaddr(fromaddr);
        if (hpeer == -1) {
            printf("** CHATTEXT from unknown text: %s fromaddr: %s/%d **\n", CSTR(text), HostAddr_ipaddress(fromaddr), ntohs(HostAddr_port(fromaddr)));
            return;
        }
        char alias[ALIAS_SIZE+1];
        char hostname[HOSTNAME_SIZE+1];
        get_peer_data(hpeer, alias, hostname, NULL, NULL);
        printf("** CHATTEXT from: %s/%s text: %s fromaddr: %s/%d **\n", alias, hostname, CSTR(text), HostAddr_ipaddress(fromaddr), ntohs(HostAddr_port(fromaddr)));

        ChatText ct;
        ct.timestamp = time(NULL);
        ct.fromaddr = fromaddr;
        ct.toaddr = 0;
        ct.text = StringDup(GChatTexts.arena, text);
        ArrayAppend(&GChatTexts, &ct);

        DataParams *dp = malloc(sizeof(DataParams));
        dp->param[0] = GINT_TO_POINTER(hpeer);
        dp->param[1] = ArrayItem(GChatTexts, GChatTexts.len-1);
        g_idle_add(IDLE_open_chatwin, dp);
    }
}

static int SendMsgV(Arena scratch, int destfd, struct timeval *timeout, char *fmt, va_list args) {
    Buffer sendbuf = BufferNew(&scratch, 256);
    NetPackLenV(&sendbuf, fmt, args);
    return NetSend_wait_until_complete(destfd, &sendbuf, timeout);
}
int SendMsg(Arena scratch, int destfd, struct timeval *timeout, char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int z = SendMsgV(scratch, destfd, timeout, fmt, args);
    va_end(args);
    return z;
}

int ConnectAndSendMsg(Arena scratch, int bindport, SockAddrIPV4 *sa, struct timeval *timeout, char *fmt, ...) {
    int destfd = OpenTcpConnectSocket(bindport, (struct sockaddr *) sa, sizeof(SockAddrIPV4), timeout);
    if (destfd == -1) {
        printf("Can't connect to host %s\n", SockAddr_ipaddress((struct sockaddr *) sa));
        return -1;
    }
    va_list args;
    va_start(args, fmt);
    int z = SendMsgV(scratch, destfd, timeout, fmt, args);
    if (z == -1)
        printf("Error sending to host %s\n", SockAddr_ipaddress((struct sockaddr *) sa));
    va_end(args);

    ShutdownSocket(destfd);
    return z;
}

void open_mainwin(Arena scratch) {
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(win), 275,425);
    gtk_window_set_position(GTK_WINDOW(win), GTK_WIN_POS_CENTER);
    gtk_window_set_title(GTK_WINDOW(win), "TinyMsg");

    // Menubar
    GtkWidget *menubar = gtk_menu_bar_new();
    GtkWidget *tmmenu = gtk_menu_new();
    GtkWidget *tmmi = gtk_menu_item_new_with_mnemonic("_TinyMsg");
    GtkWidget *settingsmi = gtk_menu_item_new_with_mnemonic("_Settings");
    GtkWidget *quitmi = gtk_menu_item_new_with_mnemonic("_Quit");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(tmmi), tmmenu);
    gtk_menu_shell_append(GTK_MENU_SHELL(tmmenu), settingsmi);
    gtk_menu_shell_append(GTK_MENU_SHELL(tmmenu), quitmi);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), tmmi);

    String s = StringFormat(&scratch, "<span foreground=\"gray\" weight=\"bold\">%s/%s</span>", CSTR(GAlias), CSTR(GHostname));
    GtkWidget *aliaslbl = create_markup_label2(CSTR(s));
    set_widget_margins(aliaslbl, 0,0,10,15);
    GtkWidget *peersframe = gtk_frame_new("Peers");
    GtkWidget *peerslb = gtk_list_box_new();
    set_widget_margins(peerslb, 5,5,2,0);
    gtk_container_add(GTK_CONTAINER(peersframe), peerslb);

    GtkWidget *contentbox = gtk_vbox_new(FALSE, 0);
    set_widget_margins(contentbox, 10,10,0,0);
    gtk_box_pack_start(GTK_BOX(contentbox), aliaslbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(contentbox), peersframe, TRUE, TRUE, 0);

    GtkWidget *framebox = gtk_vbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(framebox), menubar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(framebox), contentbox, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(win), framebox);

    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(peerslb, "row-activated", G_CALLBACK(CB_select_peer), NULL);

    GMainWin.win = win;
    GMainWin.peerslb = peerslb;

    gtk_widget_show_all(win);
}
static void CB_select_peer(GtkWidget *w, GtkListBoxRow *row, gpointer data) {
    Arena scratch = GScratch1;

    TMHandle hpeer = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "hpeer"));
    open_chatwin(scratch, hpeer);
}

// New peer appears, add to peers listbox.
static gboolean IDLE_peer_online(gpointer data) {
    Arena scratch = GScratch1;
    TMHandle hpeer = GPOINTER_TO_INT(data);

    char alias[ALIAS_SIZE+1];
    int z = get_peer_data(hpeer, alias, NULL, NULL, NULL);
    assert(z != -1);
    if (z == -1)
        strcpy(alias, "Peer");

    // Look for existing hpeer row in peers listbox
    GtkWidget *updaterow = NULL;
    for (int i=0; ; i++) {
        GtkWidget *row = GTK_WIDGET(gtk_list_box_get_row_at_index(GTK_LIST_BOX(GMainWin.peerslb), i));
        if (row == NULL)
            break;
        TMHandle row_hpeer = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "hpeer"));
        if (row_hpeer == hpeer) {
            updaterow = row;
            break;
        }
    }
    // No existing hpeer, add new row
    if (updaterow == NULL) {
        updaterow = gtk_list_box_row_new();
        g_object_set_data(G_OBJECT(updaterow), "hpeer", GINT_TO_POINTER(hpeer));
        gtk_container_add(GTK_CONTAINER(GMainWin.peerslb), updaterow);
    }

    // Set new peer alias to listbox.
    clear_controls(updaterow);
    gtk_container_add(GTK_CONTAINER(updaterow), create_label2(alias));
    gtk_widget_show_all(GMainWin.peerslb);

    // Update open chat window 
    UIChatWin *cw = find_chatwin_from_peer(hpeer);
    if (cw) {
        // Enable send text functionality
        gtk_widget_set_sensitive(cw->scrolltext, TRUE);
        gtk_widget_set_sensitive(cw->sendbtn, TRUE);
        String status = StringFormat(&scratch, "%s is online", alias);
        gtk_statusbar_push(GTK_STATUSBAR(cw->statusbar), cw->statusbar_ctxid, CSTR(status));

        // Update statusbar
        String markuptext = StringFormat(&scratch, "<span color='blue'>%s is online</span>", alias);
        GtkListBox_append(cw->msghistorylb, CSTR(markuptext));
        gtk_widget_show_all(cw->msghistorylb);
    }

    return G_SOURCE_REMOVE;
}
static gboolean IDLE_peer_offline(gpointer data) {
    Arena scratch = GScratch1;
    TMHandle hpeer = GPOINTER_TO_INT(data);

    char alias[ALIAS_SIZE+1];
    int z = get_peer_data(hpeer, alias, NULL, NULL, NULL);
    assert(z != -1);
    if (z == -1)
        strcpy(alias, "Peer");

    // Look for existing hpeer row in peers listbox
    GtkWidget *updaterow = NULL;
    for (int i=0; ; i++) {
        GtkWidget *row = GTK_WIDGET(gtk_list_box_get_row_at_index(GTK_LIST_BOX(GMainWin.peerslb), i));
        if (row == NULL)
            break;
        TMHandle row_hpeer = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "hpeer"));
        if (row_hpeer == hpeer) {
            updaterow = row;
            break;
        }
    }
    // Remove peer from listbox
    if (updaterow != NULL) {
        gtk_widget_destroy(updaterow);
        gtk_widget_show_all(GMainWin.peerslb);
    }

    // Update open chat window 
    UIChatWin *cw = find_chatwin_from_peer(hpeer);
    if (cw) {
        // Disable send text functionality
        gtk_widget_set_sensitive(cw->scrolltext, FALSE);
        gtk_widget_set_sensitive(cw->sendbtn, FALSE);
        String status = StringFormat(&scratch, "%s is offline", alias);
        gtk_statusbar_push(GTK_STATUSBAR(cw->statusbar), cw->statusbar_ctxid, CSTR(status));

        // Update statusbar
        String markuptext = StringFormat(&scratch, "<span color='darkgrey'>%s is offline</span>", alias);
        GtkListBox_append(cw->msghistorylb, CSTR(markuptext));
        gtk_widget_show_all(cw->msghistorylb);
    }

    destroy_peer(hpeer);
    return G_SOURCE_REMOVE;
}

UIChatWin *find_chatwin_from_peer(TMHandle hpeer) {
    for (int i=0; i < GChatWins.len; i++) {
        UIChatWin *cw = ArrayItem(GChatWins, i);
        if (cw->hpeer == hpeer)
            return cw;
    }
    return NULL;
}
int find_chatwin_from_peer2(TMHandle hpeer) {
    for (int i=0; i < GChatWins.len; i++) {
        UIChatWin *cw = ArrayItem(GChatWins, i);
        if (cw->hpeer == hpeer)
            return i;
    }
    return -1;
}
void add_chattext(Arena scratch, GtkWidget *msglb, ChatText *ct, char *peer_alias, char *peer_hostname, HostAddr peer_fromaddr, HostAddr peer_toaddr) {
    if (ct->fromaddr == peer_fromaddr) {
        // Received chat from peer
        String markuptext = StringFormat(&scratch, "<span color='blue' weight='bold'>%s</span>:\n%s", peer_alias, CSTR(ct->text));
        GtkListBox_append(msglb, CSTR(markuptext));
    } else if (ct->toaddr == peer_toaddr) {
        // Sent chat to peer
        String markuptext = StringFormat(&scratch, "<span color='darkgreen' weight='bold'>%s</span>:\n%s", CSTR(GAlias), CSTR(ct->text));
        GtkListBox_append(msglb, CSTR(markuptext));
    }
}

void refresh_msghistory(Arena scratch, GtkWidget *msglb, Array chattexts, char *peer_alias, char *peer_hostname, HostAddr peer_fromaddr, HostAddr peer_toaddr) {
    clear_controls(msglb);

    for (int i=0; i < chattexts.len; i++) {
        Arena tmpscratch = scratch;
        ChatText *ct = ArrayItem(chattexts, i);
        add_chattext(scratch, msglb, ct, peer_alias, peer_hostname, peer_fromaddr, peer_toaddr);
    }
}

void open_chatwin(Arena scratch, TMHandle hpeer) {
    int z;

    char peer_alias[ALIAS_SIZE+1];
    char peer_hostname[HOSTNAME_SIZE+1];
    HostAddr peer_fromaddr, peer_toaddr;
    z = get_peer_data(hpeer, peer_alias, peer_hostname, &peer_fromaddr, &peer_toaddr);
    if (z == -1)
        return;

    // Show existing chatwin if previously created
    UIChatWin *cw = find_chatwin_from_peer(hpeer);
    if (cw) {
        gtk_widget_show_all(cw->win);
        gtk_window_present(GTK_WINDOW(cw->win));
        return;
    }

    // Create new chat window for hpeer
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(win), 320,480);
    gtk_window_set_position(GTK_WINDOW(win), GTK_WIN_POS_CENTER_ON_PARENT);
    String s = StringFormat(&scratch, "%s chat", peer_alias);
    gtk_window_set_title(GTK_WINDOW(win), CSTR(s));

    GtkWidget *msghistorylb = gtk_list_box_new();
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(msghistorylb), FALSE);
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(msghistorylb), GTK_SELECTION_NONE);
    GtkWidget *scrolllb = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(scrolllb), msghistorylb);

    GtkWidget *sendcaption = create_label("Send Message");
    GtkWidget *sendtext = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(sendtext), GTK_WRAP_WORD);
    GtkWidget *scrolltext = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolltext), GTK_POLICY_AUTOMATIC, GTK_POLICY_ALWAYS);
    gtk_widget_set_size_request(scrolltext, -1, 80);
    gtk_container_add(GTK_CONTAINER(scrolltext), sendtext);

    GtkWidget *sendbtn = gtk_button_new_with_mnemonic("_Send");
    GtkWidget *hbox = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_end(GTK_BOX(hbox), sendbtn, FALSE, FALSE, 0);

    GtkWidget *statusbar = gtk_statusbar_new();
    guint statusbar_ctxid = gtk_statusbar_get_context_id(GTK_STATUSBAR(statusbar), "sb");
    String introtext = StringFormat(&scratch, "Chatting with %s", peer_alias);
    gtk_statusbar_push(GTK_STATUSBAR(statusbar), statusbar_ctxid, CSTR(introtext));

    GtkWidget *contentbox = gtk_vbox_new(FALSE, 0);
    set_widget_margins(contentbox, 10,10,5,5);
    gtk_box_pack_start(GTK_BOX(contentbox), scrolllb, TRUE, TRUE, 10);
    gtk_box_pack_start(GTK_BOX(contentbox), sendcaption, FALSE, FALSE, 5);
    gtk_box_pack_start(GTK_BOX(contentbox), scrolltext, FALSE, FALSE, 3);
    gtk_box_pack_start(GTK_BOX(contentbox), hbox, FALSE, FALSE, 3);

    GtkWidget *framebox = gtk_vbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(framebox), contentbox, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(framebox), statusbar, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(win), framebox);

    refresh_msghistory(scratch, msghistorylb, GChatTexts, peer_alias, peer_hostname, peer_fromaddr, peer_toaddr);

    g_signal_connect(sendbtn, "clicked", G_CALLBACK(CB_chatwin_send), GINT_TO_POINTER(hpeer));
    //g_signal_connect(win, "destroy", G_CALLBACK(CB_chatwin_destroy), GINT_TO_POINTER(hpeer));
    g_signal_connect(win, "delete-event", G_CALLBACK(CB_chatwin_delete), GINT_TO_POINTER(hpeer));

    UIChatWin new_cw;
    new_cw.hpeer = hpeer;
    new_cw.win = win;
    new_cw.msghistorylb = msghistorylb;
    new_cw.sendtext = sendtext;
    new_cw.scrolltext = scrolltext;
    new_cw.statusbar = statusbar;
    new_cw.statusbar_ctxid = statusbar_ctxid;
    new_cw.sendbtn = sendbtn;
    ArrayAppend(&GChatWins, &new_cw);

    gtk_widget_show_all(win);
}
static gboolean CB_chatwin_delete(GtkWidget *w, GdkEvent *e, gpointer data) {
    TMHandle hpeer = GPOINTER_TO_INT(data);
    gtk_widget_hide(w);
    return TRUE;
}
static void CB_chatwin_destroy(GtkWidget *w, gpointer data) {
    TMHandle hpeer = GPOINTER_TO_INT(data);
    int icw = find_chatwin_from_peer2(hpeer);
    if (icw != -1) {
        ArrayRemove(&GChatWins, icw);
        if (GChatWins.len == 0) {
            Arena *arena_chatwins = GChatWins.arena;
            ArenaReset(arena_chatwins);
            GChatWins = ArrayNew(arena_chatwins, 64, sizeof(UIChatWin));
        }
    }
}
static void CB_chatwin_send(GtkWidget *w, gpointer data) {
    Arena scratch = GScratch1;
    TMHandle hpeer = GPOINTER_TO_INT(data);

    char peer_alias[ALIAS_SIZE+1];
    char peer_hostname[HOSTNAME_SIZE+1];
    HostAddr peer_fromaddr=0, peer_toaddr=0;
    int z = get_peer_data(hpeer, peer_alias, peer_hostname, &peer_fromaddr, &peer_toaddr);
    if (z == -1)
        return;

    UIChatWin *cw = find_chatwin_from_peer(hpeer);
    if (cw == NULL)
        return;

    SockAddrIPV4 peer_to_sa = SockAddrFromHostAddr(peer_toaddr);
    struct timeval timeout = {2,0};
    z = ConnectAndSendMsg(scratch, GSendPort, &peer_to_sa, &timeout, "%b%s", CHATTEXT, GtkTextView_gettext(GTK_TEXT_VIEW(cw->sendtext)));
    if (z == -1) {
        String err = StringFormat(&scratch, "Send error: %s\n", strerror(errno));
        gtk_statusbar_push(GTK_STATUSBAR(cw->statusbar), cw->statusbar_ctxid, CSTR(err));
        return;
    }

    ChatText ct;
    ct.timestamp = time(NULL);
    ct.fromaddr = 0;
    ct.toaddr = peer_toaddr;
    ct.text = StringNew(GChatTexts.arena, GtkTextView_gettext(GTK_TEXT_VIEW(cw->sendtext)));
    ArrayAppend(&GChatTexts, &ct);

    add_chattext(scratch, cw->msghistorylb, &ct, peer_alias, peer_hostname, peer_fromaddr, peer_toaddr);

    GtkTextBuffer *tb = gtk_text_view_get_buffer(GTK_TEXT_VIEW(cw->sendtext));
    gtk_text_buffer_set_text(tb, "", -1);
    gtk_widget_show_all(cw->win);
}

static gboolean IDLE_open_chatwin(gpointer data) {
    Arena scratch = GScratch1;
    DataParams *dp = data;
    TMHandle hpeer = GPOINTER_TO_INT(dp->param[0]);
    ChatText *ct = dp->param[1];
    free(dp);

    UIChatWin *cw = find_chatwin_from_peer(hpeer);
    if (cw) {
        char peer_alias[ALIAS_SIZE+1];
        char peer_hostname[HOSTNAME_SIZE+1];
        HostAddr peer_fromaddr, peer_toaddr;
        int z = get_peer_data(hpeer, peer_alias, peer_hostname, &peer_fromaddr, &peer_toaddr);
        if (z != -1) {
            add_chattext(scratch, cw->msghistorylb, ct, peer_alias, peer_hostname, peer_fromaddr, peer_toaddr);
            gtk_widget_show_all(cw->msghistorylb);
            gtk_window_present(GTK_WINDOW(cw->win));
            return G_SOURCE_REMOVE;
        }
    }

    open_chatwin(scratch, hpeer);
    return G_SOURCE_REMOVE;
}

