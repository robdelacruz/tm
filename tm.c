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

void sigint(int sig);
void parse_args(int argc, char **argv);
int execute_command(Arena scratch, char *cmd);
void run_shell(Arena scratch);
void create_ui(Arena scratch);

void open_chatwin(Arena scratch, TMHandle hpeer);
static void CB_chatwin_destroy(GtkWidget *w, gpointer data);
static void CB_chatwin_send(GtkWidget *w, gpointer data);
void add_chattext(Arena scratch, GtkWidget *lb, ChatText *chattext, HostAddr peer_fromaddr, HostAddr peer_toaddr);

void* THREAD_wait_for_tcp_messages(void *data);
void handle_msg(Arena scratch, int fd, HostAddr fromaddr, char *msgbytes, u16 msglen, Array *socketctxs, fd_set *writefds, int *maxfd);

int SendMsg(int destfd, int bufsize, struct timeval *timeout, char *fmt, ...);

static void CB_mainwin_destroy(GtkWidget *w, gpointer data);
static void CB_select_peer(GtkWidget *w, GtkListBoxRow *row, gpointer data);
static gboolean IDLE_peer_online(gpointer data);

typedef struct {
    GtkWidget *peerslistbox;
} TMUI;

typedef struct {
    TMHandle hpeer;
    GtkWidget *win;
    GtkWidget *msghistorylb;
    GtkWidget *sendtext;
} ChatWin;

String GTrackerHost;
u16 GTrackerPort = TRACKER_PORT;
u16 GSendPort = SEND_PORT;
u16 GListenPort = LISTEN_PORT;
String GAlias;
String GHostname;
int GTrackerFD=0;
HostAddr GTrackerAddr=0;

Array GChatTexts;
Array GChatWins;

int GUnblockSelect_writefd=0;

TMUI GUI = {0};

int main(int argc, char *argv[]) {
    int z;
    Arena arena = ArenaNew(64*1024);
    Arena scratch = ArenaNew(4*1024);

    gtk_init(&argc, &argv);
    init_tmdata(&arena, scratch);

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

    GChatTexts = ArrayNew(&arena, 64, sizeof(ChatText));
    GChatWins = ArrayNew(&arena, 64, sizeof(ChatWin));

    struct timeval timeout = {2, 0};
    GTrackerFD = OpenTcpConnectSocket3(GSendPort, CSTR(GTrackerHost), GTrackerPort, &timeout);
    if (GTrackerFD == -1) {
        printf("Can't connect to tracker %s/%d\n", CSTR(GTrackerHost), GTrackerPort);
        exit(1);
    }

    pthread_t thread_wait_tcp;
    pthread_create(&thread_wait_tcp, NULL, THREAD_wait_for_tcp_messages, NULL);
    //GThread *thread_wait_tcp = g_thread_new("wait_for_tcp_messages", THREAD_wait_for_tcp_messages, NULL);

    z = SendMsg(GTrackerFD, 128, &timeout, "%b%s%s%w", KNOCK, CSTR(GAlias), CSTR(GHostname), GListenPort);
    if (z == -1) {
        printf("Error sending knock to tracker %s/%d\n", CSTR(GTrackerHost), GTrackerPort);
        return 1;
    }

    create_ui(scratch);
    gtk_main();

    // Break out of select() loop in THREAD_wait_for_tcp_messages().
    char writebuf[] = "1";
    z = write(GUnblockSelect_writefd, writebuf, sizeof(writebuf));
    if (z == -1)
        fprintf(stderr, "write(): %s\n", strerror(errno));
    assert(z > 0);

    SendMsg(GTrackerFD, 10, &timeout, "%b", BYE);
    ShutdownSocket(GTrackerFD);

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
    struct timeval timeout = {2, 0};
    SendMsg(GTrackerFD, 10, &timeout, "%b", BYE);
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
                print_chattexts(GChatTexts);
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
            struct timeval timeout = {2,0};
            int peerfd = OpenTcpConnectSocket2(GSendPort, toaddr, &timeout);
            if (peerfd == -1) {
                printf("Can't connect to peer %s (%s/%d)\n", CSTR(sendalias), HostAddr_ipaddress(toaddr), ntohs(HostAddr_port(toaddr)));
                return 0;
            }
            Buffer sendbuf = BufferNew(&scratch, 255);
            NetPackLen(&sendbuf, "%b%s", CHATTEXT, CSTR(chattext));
            if (NetSend_wait_until_complete(peerfd, &sendbuf, &timeout) == -1)
                printf("Error sending to peer %s (%s/%d)\n", CSTR(sendalias), HostAddr_ipaddress(toaddr), ntohs(HostAddr_port(toaddr)));

            ShutdownSocket(peerfd);
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
    } else if (msgno == PEER_OFFLINE) {
        // Should only come from tracker
        if (!HostAddr_ipaddr_equals(fromaddr, GTrackerAddr))
            return;

        HostAddr peer_fromaddr;
        NetUnpack(msgbytes, msglen, "%b%L", &msgno, &peer_fromaddr);

        printf("** PEER_OFFLINE fromaddr: %s/%d **\n", HostAddr_ipaddress(peer_fromaddr), ntohs(HostAddr_port(peer_fromaddr)));

        TMHandle hpeer = find_peer_fromaddr(peer_fromaddr);
        if (hpeer != -1) {
            destroy_peer(hpeer);
            //GtkListBox_remove(GUI.peerslistbox, ipeer);
        }
        print_peers();
    } else if (msgno == CHATTEXT) {
        String text = StringNew0(&scratch);
        NetUnpack(msgbytes, msglen, "%b%s", &msgno, &text);

        TMHandle hpeer = find_peer_fromaddr(fromaddr);
        if (hpeer == -1) {
            printf("** CHATTEXT from unknown text: %s fromaddr: %s/%d **\n", CSTR(text), HostAddr_ipaddress(fromaddr), ntohs(HostAddr_port(fromaddr)));
            return;
        }
        String alias = StringNew0(&scratch);
        String hostname = StringNew0(&scratch);
        get_peer_data(hpeer, &alias, &hostname, NULL, NULL);
        printf("** CHATTEXT from: %s/%s text: %s fromaddr: %s/%d **\n", CSTR(alias), CSTR(hostname), CSTR(text), HostAddr_ipaddress(fromaddr), ntohs(HostAddr_port(fromaddr)));

        ChatText ct;
        ct.timestamp = time(NULL);
        ct.alias = StringDup(GChatTexts.arena, alias);
        ct.hostname = StringDup(GChatTexts.arena, hostname);
        ct.fromaddr = fromaddr;
        ct.toaddr = 0;
        ct.text = StringDup(GChatTexts.arena, text);
        ArrayAppend(&GChatTexts, &ct);
    }
}

int SendMsgV(int destfd, int bufsize, struct timeval *timeout, char *fmt, va_list args) {
    char buf[bufsize];
    Buffer sendbuf = BUFFER(buf, bufsize);
    NetPackLenV(&sendbuf, fmt, args);

    return NetSend_wait_until_complete(destfd, &sendbuf, timeout);
}
int SendMsg(int destfd, int bufsize, struct timeval *timeout, char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int z = SendMsgV(destfd, bufsize, timeout, fmt, args);
    va_end(args);
    return z;
}

void create_ui(Arena scratch) {
    GtkWidget *mainwin = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(mainwin), 275,425);
    gtk_window_set_position(GTK_WINDOW(mainwin), GTK_WIN_POS_CENTER);
    gtk_window_set_title(GTK_WINDOW(mainwin), "TinyMsg");

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
    GtkWidget *peerslistbox = gtk_list_box_new();
    set_widget_margins(peerslistbox, 5,5,2,0);
    gtk_container_add(GTK_CONTAINER(peersframe), peerslistbox);

    GtkWidget *contentbox = gtk_vbox_new(FALSE, 0);
    set_widget_margins(contentbox, 10,10,0,0);
    gtk_box_pack_start(GTK_BOX(contentbox), aliaslbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(contentbox), peersframe, TRUE, TRUE, 0);

    GtkWidget *framebox = gtk_vbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(framebox), menubar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(framebox), contentbox, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(mainwin), framebox);

    g_signal_connect(mainwin, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(peerslistbox, "row-activated", G_CALLBACK(CB_select_peer), NULL);

    GUI.peerslistbox = peerslistbox;

    gtk_widget_show_all(mainwin);
}

static void CB_mainwin_destroy(GtkWidget *w, gpointer data) {
    gtk_main_quit();
}

// New peer appears, add to peers listbox.
static gboolean IDLE_peer_online(gpointer data) {
    u8 arenabytes[512];
    Arena scratch = ArenaNewAuto(arenabytes, sizeof(arenabytes));

    TMHandle hpeer = GPOINTER_TO_INT(data);
    String alias = StringNew0(&scratch);
    int z = get_peer_data(hpeer, &alias, NULL, NULL, NULL);
    if (z == -1)
        return G_SOURCE_REMOVE;

    GtkWidget *updaterow = NULL;

    // Look for existing hpeer row in peers listbox
    for (int i=0; ; i++) {
        GtkWidget *row = GTK_WIDGET(gtk_list_box_get_row_at_index(GTK_LIST_BOX(GUI.peerslistbox), i));
        if (row == NULL)
            break;
        TMHandle row_hpeer = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "hpeer"));
        if (row_hpeer == hpeer) {
            updaterow = row;
            clear_controls(updaterow);
            break;
        }
    }

    // No existing hpeer, add new row
    if (updaterow == NULL)
        updaterow = gtk_list_box_row_new();

    gtk_container_add(GTK_CONTAINER(updaterow), create_label2(CSTR(alias)));
    g_object_set_data(G_OBJECT(updaterow), "hpeer", GINT_TO_POINTER(hpeer));
    gtk_container_add(GTK_CONTAINER(GUI.peerslistbox), updaterow);

    gtk_widget_show_all(GUI.peerslistbox);
    return G_SOURCE_REMOVE;
}

static void CB_select_peer(GtkWidget *w, GtkListBoxRow *row, gpointer data) {
    u8 arenabytes[1024];
    Arena scratch = ArenaNewAuto(arenabytes, sizeof(arenabytes));

    TMHandle hpeer = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "hpeer"));
    open_chatwin(scratch, hpeer);
}

void open_chatwin(Arena scratch, TMHandle hpeer) {
    // Show existing chatwin if previously created
    for (int i=0; i < GChatWins.len; i++) {
        ChatWin *cw = ArrayItem(GChatWins, i);
        if (cw->hpeer == hpeer) {
            gtk_window_present(GTK_WINDOW(cw->win));
            return;
        }
    }

    String alias = StringNew0(&scratch);
    HostAddr fromaddr, toaddr;
    int z = get_peer_data(hpeer, &alias, NULL, &fromaddr, &toaddr);
    if (z == -1)
        return;

    // Create new chat window for hpeer
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(win), 320,480);
    gtk_container_set_border_width(GTK_CONTAINER(win), 10);
    String s = StringFormat(&scratch, "%s chat", CSTR(alias));
    gtk_window_set_title(GTK_WINDOW(win), CSTR(s));

    GtkWidget *msghistorylb = gtk_list_box_new();
    GtkWidget *scrolllb = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(scrolllb), msghistorylb);

    GtkWidget *sendcaption = create_label("Send Message");
    GtkWidget *sendtext = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(sendtext), GTK_WRAP_WORD);
    GtkWidget *scrolltext = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_size_request(scrolltext, -1, 80);
    gtk_container_add(GTK_CONTAINER(scrolltext), sendtext);

    GtkWidget *sendbtn = gtk_button_new_with_mnemonic("_Send");
    GtkWidget *hbox = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_end(GTK_BOX(hbox), sendbtn, FALSE, FALSE, 0);

    GtkWidget *contentbox = gtk_vbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(contentbox), scrolllb, TRUE, TRUE, 10);
    gtk_box_pack_start(GTK_BOX(contentbox), sendcaption, FALSE, FALSE, 5);
    gtk_box_pack_start(GTK_BOX(contentbox), scrolltext, FALSE, FALSE, 3);
    gtk_box_pack_start(GTK_BOX(contentbox), hbox, FALSE, FALSE, 3);
    gtk_container_add(GTK_CONTAINER(win), contentbox);

    for (int i=0; i < GChatTexts.len; i++) {
        ChatText *chattext = ArrayItem(GChatTexts, i);
        add_chattext(scratch, msghistorylb, chattext, fromaddr, toaddr);
    }

    g_signal_connect(sendbtn, "clicked", G_CALLBACK(CB_chatwin_send), GINT_TO_POINTER(hpeer));
    g_signal_connect(win, "destroy", G_CALLBACK(CB_chatwin_destroy), GINT_TO_POINTER(hpeer));

    ChatWin cw;
    cw.hpeer = hpeer;
    cw.win = win;
    cw.msghistorylb = msghistorylb;
    cw.sendtext = sendtext;
    ArrayAppend(&GChatWins, &cw);

    gtk_widget_show_all(win);
}

static void CB_chatwin_destroy(GtkWidget *w, gpointer data) {
    TMHandle hpeer = GPOINTER_TO_INT(data);

    for (int i=0; i < GChatWins.len; i++) {
        ChatWin *cw = ArrayItem(GChatWins, i);
        if (cw->hpeer == hpeer) {
            ArrayRemove(&GChatWins, i);
            return;
        }
    }
}
static void CB_chatwin_send(GtkWidget *w, gpointer data) {
    u8 arenabytes[512];
    Arena scratch = ArenaNewAuto(arenabytes, sizeof(arenabytes));

    TMHandle hpeer = GPOINTER_TO_INT(data);

    String sendalias = StringNew0(&scratch);
    HostAddr toaddr=0;
    int z = get_peer_data(hpeer, &sendalias, NULL, NULL, &toaddr);
    if (z == -1)
        return;

    ChatWin *cw = NULL;
    for (int i=0; i < GChatWins.len; i++) {
        ChatWin *p = ArrayItem(GChatWins, i);
        if (p->hpeer == hpeer) {
            cw = p;
            break;
        }
    }
    if (cw == NULL)
        return;

    struct timeval timeout = {2,0};
    int peerfd = OpenTcpConnectSocket2(GSendPort, toaddr, &timeout);
    if (peerfd == -1) {
        printf("Can't connect to peer (%s/%d)\n", HostAddr_ipaddress(toaddr), ntohs(HostAddr_port(toaddr)));
        return;
    }

    char bufbytes[1024];
    Buffer sendbuf = BUFFER(bufbytes, sizeof(bufbytes));
    NetPackLen(&sendbuf, "%b%s", CHATTEXT, GtkTextView_gettext(GTK_TEXT_VIEW(cw->sendtext)));
    if (NetSend_wait_until_complete(peerfd, &sendbuf, &timeout) == -1)
        printf("Error sending to peer (%s/%d)\n", HostAddr_ipaddress(toaddr), ntohs(HostAddr_port(toaddr)));
    ShutdownSocket(peerfd);
}

void add_chattext(Arena scratch, GtkWidget *lb, ChatText *chattext, HostAddr peer_fromaddr, HostAddr peer_toaddr) {
    if (chattext->fromaddr == peer_fromaddr) {
        String markuptext = StringFormat(&scratch, "<b>%s</b>:\n%s", CSTR(chattext->alias), CSTR(chattext->text));
        GtkListBox_append(lb, CSTR(markuptext));
    } else if (chattext->toaddr == peer_toaddr) {
        String markuptext = StringFormat(&scratch, "<b>%s</b>:\n%s", CSTR(GAlias), CSTR(chattext->text));
        GtkListBox_append(lb, CSTR(markuptext));
    }
}

