#include <gtk/gtk.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <signal.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "clib.h"
#include "cnet.h"
#include "msg.h"
#include "uistuff.h"

typedef enum {
    NONE=0,
    LOGINWIN=1,
    REGISTERWIN=2,
    CONTACTSWIN=3,
    INVITEWIN
} WindowID;

typedef struct {
    WindowID active_win;
    GtkWidget *mainwin;
    GtkWidget *statusbar;
    String statusbar_text;

    GtkWidget *login_txtusername;
    GtkWidget *login_txtpassword;
    GtkWidget *login_chkremember;

    GtkWidget *register_txtusername;
    GtkWidget *register_txtpassword;
    GtkWidget *register_txtpassword2;

    GtkWidget *contacts_list;

    GtkWidget *invite_txtsearch;
} UIState;

typedef struct {
    String username;
    String tok;
} Session;

typedef void (*MSGCALLBACK)(char *msgbytes, u16 len);

static int connect_to_server(char *serverhost, char *serverport, struct timeval *timeout_val);
static int connect_to_server2(char *serverhost, char *serverport, struct timeval *timeout_val, int *serverfd);
static void send_and_wait_for_response(int fd, Buffer *buf, struct timeval *timeout_val, MSGCALLBACK on_recv_msg);
static int wait_for_message_to_be_sent(int fd, Buffer *writebuf, struct timeval *timeout_val);
static int wait_for_response_message(int fd, struct timeval *timeout_val, MSGCALLBACK on_recv_msg);

static gboolean IDLE_enable_window(gpointer data);
static gboolean IDLE_disable_window(gpointer data);
static void update_connect_fail_ui();

static void create_login_ui(char *username, char *password, gboolean autologin);
static void CALLBACK_login_clicked(GtkWidget *w, gpointer data);
static gpointer THREAD_login(gpointer data);

static void create_register_ui();
static void CALLBACK_register_clicked(GtkWidget *w, gpointer data);
static gpointer THREAD_register(gpointer data);

static void create_contacts_ui();
static GtkWidget *create_contact_label(char *username);
static gpointer THREAD_refresh_contacts(gpointer data);

static void create_invite_ui();
static void CALLBACK_search_users(GtkWidget *w, gpointer data);
static gpointer THREAD_search_users(gpointer data);

static void CALLBACK_menu_login(GtkWidget *w, gpointer data);
static void CALLBACK_menu_logout(GtkWidget *w, gpointer data);
static void CALLBACK_menu_register(GtkWidget *w, gpointer data);
static void CALLBACK_menu_contacts(GtkWidget *w, gpointer data);
static void CALLBACK_menu_invite(GtkWidget *w, gpointer data);

static void on_read_eof();
static void on_server_close();
static void on_received_msg(char *msgbytes, u16 len);

static gboolean SF_show_connect_error(gpointer data);

static gboolean IDLE_LoginUserResponse(gpointer data);
static gboolean IDLE_RegisterUserResponse(gpointer data);
static gboolean IDLE_SearchUsernameResponse(gpointer data);

int G_serverfd = -1;
char *G_serverhost = "localhost";
char *G_serverport = "8000";
GtkWidget *G_mainwin = NULL;
UIState G_ui = {0};
Session G_session = {0};
struct timeval G_connect_timeout_val = {8, 0};
struct timeval G_timeout_val = {2, 0};

fd_set G_readfds, G_writefds;
int G_maxfd=0;

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    if (argc > 1)
        G_serverhost = argv[1];
    if (argc > 2)
        G_serverport = argv[2];

    // Main window
    G_mainwin = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(G_mainwin), 275,425);
    gtk_window_set_position(GTK_WINDOW(G_mainwin), GTK_WIN_POS_CENTER);
    gtk_window_set_title(GTK_WINDOW(G_mainwin), "RobChat");
    g_signal_connect(G_mainwin, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    char username[512] = "";
    char password[512] = "";
    gboolean autologin = FALSE;

    FILE *f = fopen("cserv_session.txt", "r");
    if (f != NULL) {
        if (fgets(username, sizeof(username), f) != NULL)
            fgets(password, sizeof(password), f);
        fclose(f);

        if (strlen(username) > 0) {
            // Remove trailing \n read by fgets()
            if (username[strlen(username)-1] == '\n')
                username[strlen(username)-1] = 0;
            if (strlen(password) > 0 && password[strlen(password)-1] == '\n')
                password[strlen(password)-1] = 0;

            autologin = TRUE;
        }
    }

    create_login_ui(username, password, autologin);

    gtk_main();
    return 0;
}

static gboolean IDLE_enable_window(gpointer data) {
    set_statusbar_message(GTK_STATUSBAR(G_ui.statusbar), 0, G_ui.statusbar_text.bs);
    gtk_widget_set_sensitive(G_mainwin, TRUE);
    return G_SOURCE_REMOVE;
}
static gboolean IDLE_disable_window(gpointer data) {
    set_statusbar_message(GTK_STATUSBAR(G_ui.statusbar), 0, G_ui.statusbar_text.bs);
    gtk_widget_set_sensitive(G_mainwin, FALSE);
    return G_SOURCE_REMOVE;
}
static void update_connect_fail_ui() {
    StringAssignFormat(&G_ui.statusbar_text, "Can't connect to '%s'", G_serverhost);
    g_idle_add(IDLE_enable_window, NULL);
}

static void create_login_ui(char *username, char *password, gboolean autologin) {
    clear_controls(G_mainwin);

    // Menu and statusbar
    GtkWidget *menubar = gtk_menu_bar_new();
    GtkWidget *filemenu = gtk_menu_new();
    GtkWidget *filemi = gtk_menu_item_new_with_mnemonic("_File");
    GtkWidget *registermi = gtk_menu_item_new_with_mnemonic("_Register");
    GtkWidget *quitmi = gtk_menu_item_new_with_mnemonic("_Quit");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(filemi), filemenu);
    gtk_menu_shell_append(GTK_MENU_SHELL(filemenu), registermi);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), filemi);
    GtkWidget *statusbar = gtk_statusbar_new();

    // Login controls
    GtkWidget *lbl1 = create_label1("Username");
    GtkWidget *txtusername = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(txtusername), username);
    GtkWidget *lbl2 = create_label1("Password");
    GtkWidget *txtpassword = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(txtpassword), password);
    GtkWidget *chkremember = gtk_check_button_new_with_mnemonic("_Remember me");
    GtkWidget *btnbox = gtk_vbox_new(FALSE, 0);
    GtkWidget *loginbtn = create_center_button("Login");

    GtkWidget *formbox = gtk_vbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(formbox), lbl1, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(formbox), txtusername, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(formbox), lbl2, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(formbox), txtpassword, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(formbox), chkremember, FALSE, FALSE, 5);
    gtk_box_pack_start(GTK_BOX(btnbox), loginbtn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(formbox), btnbox, FALSE, FALSE, 10);
    gtk_widget_set_halign(formbox, GTK_ALIGN_CENTER);

    GtkWidget *contentbox = gtk_vbox_new(FALSE, 0);
    gtk_box_set_center_widget(GTK_BOX(contentbox), formbox);

    GtkWidget *framebox = gtk_vbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(framebox), menubar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(framebox), contentbox, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(framebox), statusbar, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(G_mainwin), framebox);

    G_ui.statusbar = statusbar;

    G_ui.active_win = LOGINWIN;
    G_ui.login_txtusername = txtusername;
    G_ui.login_txtpassword = txtpassword;
    G_ui.login_chkremember = chkremember;

    g_signal_connect(G_OBJECT(quitmi), "activate", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(G_OBJECT(registermi), "activate", G_CALLBACK(CALLBACK_menu_register), NULL);
    g_signal_connect(G_OBJECT(loginbtn), "clicked", G_CALLBACK(CALLBACK_login_clicked), NULL);

    gtk_widget_set_sensitive(G_mainwin, TRUE);
    gtk_widget_show_all(G_mainwin);

    remove("cserv_session.tmp");

    if (autologin)
        g_thread_new("THREAD_login", THREAD_login, NULL);
}
static void CALLBACK_login_clicked(GtkWidget *w, gpointer data) {
    remove("cserv_session.txt");

    gboolean is_remember_me = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(G_ui.login_chkremember));
    if (is_remember_me) {
        char *username = (char *) gtk_entry_get_text(GTK_ENTRY(G_ui.login_txtusername));
        char *password = (char *) gtk_entry_get_text(GTK_ENTRY(G_ui.login_txtpassword));

        FILE *f = fopen("cserv_session.tmp", "w");
        if (f) {
            fprintf(f, "%s\n", username);
            fprintf(f, "%s\n", password);
            fclose(f);
        }
    }

    g_thread_new("THREAD_login", THREAD_login, NULL);
}
static gpointer THREAD_login(gpointer data) {
    if (connect_to_server2(G_serverhost, G_serverport, &G_connect_timeout_val, &G_serverfd) == -1)
        return NULL;
    assert(G_serverfd != -1);

    StringAssignFormat(&G_ui.statusbar_text, "Logging in...");
    g_idle_add(IDLE_disable_window, NULL);

    Buffer writebuf = BufferNew(256);

    char *username = (char *) gtk_entry_get_text(GTK_ENTRY(G_ui.login_txtusername));
    char *password = (char *) gtk_entry_get_text(GTK_ENTRY(G_ui.login_txtpassword));
    u8 msgno = LOGINUSER_REQUEST;
    NetPackLen(&writebuf, "%b%s%s", msgno, username, password);
    send_and_wait_for_response(G_serverfd, &writebuf, &G_timeout_val, on_received_msg);

    BufferFree(&writebuf);
    return NULL;
}

static void create_register_ui() {
    clear_controls(G_mainwin);

    // Menu and statusbar
    GtkWidget *menubar = gtk_menu_bar_new();
    GtkWidget *filemenu = gtk_menu_new();
    GtkWidget *filemi = gtk_menu_item_new_with_mnemonic("_File");
    GtkWidget *loginmi = gtk_menu_item_new_with_mnemonic("_Login");
    GtkWidget *quitmi = gtk_menu_item_new_with_mnemonic("_Quit");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(filemi), filemenu);
    gtk_menu_shell_append(GTK_MENU_SHELL(filemenu), loginmi);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), filemi);
    GtkWidget *statusbar = gtk_statusbar_new();

    // Register controls
    GtkWidget *lbl1 = create_label1("Username");
    GtkWidget *txtusername = gtk_entry_new();
    GtkWidget *lbl2 = create_label1("Password");
    GtkWidget *txtpassword = gtk_entry_new();
    GtkWidget *lbl3 = create_label1("Re-enter password");
    GtkWidget *txtpassword2 = gtk_entry_new();
    GtkWidget *btnbox = gtk_vbox_new(FALSE, 0);
    GtkWidget *registerbtn = create_center_button("Register");

    GtkWidget *formbox = gtk_vbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(formbox), lbl1, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(formbox), txtusername, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(formbox), lbl2, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(formbox), txtpassword, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(formbox), lbl3, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(formbox), txtpassword2, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btnbox), registerbtn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(formbox), btnbox, FALSE, FALSE, 10);
    gtk_widget_set_halign(formbox, GTK_ALIGN_CENTER);

    GtkWidget *contentbox = gtk_vbox_new(FALSE, 0);
    gtk_box_set_center_widget(GTK_BOX(contentbox), formbox);

    GtkWidget *framebox = gtk_vbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(framebox), menubar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(framebox), contentbox, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(framebox), statusbar, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(G_mainwin), framebox);

    G_ui.statusbar = statusbar;

    G_ui.active_win = REGISTERWIN;
    G_ui.register_txtusername = txtusername;
    G_ui.register_txtpassword = txtpassword;
    G_ui.register_txtpassword2 = txtpassword2;

    g_signal_connect(G_OBJECT(quitmi), "activate", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(G_OBJECT(loginmi), "activate", G_CALLBACK(CALLBACK_menu_login), NULL);
    g_signal_connect(G_OBJECT(registerbtn), "clicked", G_CALLBACK(CALLBACK_register_clicked), NULL);

    gtk_widget_set_sensitive(G_mainwin, TRUE);
    gtk_widget_show_all(G_mainwin);
}
static void CALLBACK_register_clicked(GtkWidget *w, gpointer data) {
    g_thread_new("THREAD_register", THREAD_register, NULL);
}
static gpointer THREAD_register(gpointer data) {
    if (connect_to_server2(G_serverhost, G_serverport, &G_connect_timeout_val, &G_serverfd) == -1)
        return NULL;
    assert(G_serverfd != -1);

    StringAssignFormat(&G_ui.statusbar_text, "Sending request...");
    g_idle_add(IDLE_disable_window, NULL);

    Buffer writebuf = BufferNew(256);

    char *username = (char *) gtk_entry_get_text(GTK_ENTRY(G_ui.register_txtusername));
    char *password = (char *) gtk_entry_get_text(GTK_ENTRY(G_ui.register_txtpassword));
    char *password2 = (char *) gtk_entry_get_text(GTK_ENTRY(G_ui.register_txtpassword2));
    u8 msgno = REGISTERUSER_REQUEST;
    NetPackLen(&writebuf, "%b%s%s", msgno, username, password);
    send_and_wait_for_response(G_serverfd, &writebuf, &G_timeout_val, on_received_msg);

    BufferFree(&writebuf);
    return NULL;
}

static void create_contacts_ui() {
    clear_controls(G_mainwin);

    // Menu and statusbar
    GtkWidget *menubar = gtk_menu_bar_new();
    GtkWidget *filemenu = gtk_menu_new();
    GtkWidget *filemi = gtk_menu_item_new_with_mnemonic("_File");
    GtkWidget *logoutmi = gtk_menu_item_new_with_mnemonic("_Logout");
    GtkWidget *invitemi = gtk_menu_item_new_with_mnemonic("_Invite");
    GtkWidget *quitmi = gtk_menu_item_new_with_mnemonic("_Quit");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(filemi), filemenu);
    gtk_menu_shell_append(GTK_MENU_SHELL(filemenu), logoutmi);
    gtk_menu_shell_append(GTK_MENU_SHELL(filemenu), invitemi);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), filemi);
    GtkWidget *statusbar = gtk_statusbar_new();

    // Contacts controls
    String s = StringNew0();
    StringAssignFormat(&s, "<b><big>%s</big></b>", CSTR(G_session.username));
    GtkWidget *lbluser = create_label1("");
    gtk_label_set_markup(GTK_LABEL(lbluser), s.bs);
    StringFree(&s);

    GtkWidget *list = gtk_list_box_new();
    GtkWidget *listframe = gtk_frame_new("");
    gtk_container_add(GTK_CONTAINER(listframe), list);
    set_widget_margins(listframe, 2,2, 2,2);

    GtkWidget *contentbox = gtk_vbox_new(FALSE, 0);
    set_widget_margins(contentbox, 5, 5, 5, 5);
    gtk_box_pack_start(GTK_BOX(contentbox), lbluser, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(contentbox), listframe, TRUE, TRUE, 0);

    // abcuser, buddy123, hey_snoopy
    GtkWidget *lbl = create_contact_label("⚪ <span style=\"italic\" foreground=\"darkgrey\">abcuser</span>");
    gtk_container_add(GTK_CONTAINER(list), lbl);
    lbl = create_contact_label("🟢 buddy123");
    gtk_container_add(GTK_CONTAINER(list), lbl);
    lbl = create_contact_label("🟢 hey_snoopy");
    gtk_container_add(GTK_CONTAINER(list), lbl);

    GtkWidget *framebox = gtk_vbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(framebox), menubar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(framebox), contentbox, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(framebox), statusbar, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(G_mainwin), framebox);

    G_ui.statusbar = statusbar;

    G_ui.active_win = CONTACTSWIN;
    G_ui.contacts_list = list;

    StringFree(&s);

    g_signal_connect(G_OBJECT(quitmi), "activate", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(G_OBJECT(logoutmi), "activate", G_CALLBACK(CALLBACK_menu_logout), NULL);
    g_signal_connect(G_OBJECT(invitemi), "activate", G_CALLBACK(CALLBACK_menu_invite), NULL);

    g_thread_new("THREAD_refresh_contacts", THREAD_refresh_contacts, NULL);

    gtk_widget_set_sensitive(G_mainwin, TRUE);
    gtk_widget_show_all(G_mainwin);
}
static GtkWidget *create_contact_label(char *username) {
    String s = StringNew0();
    StringAssignFormat(&s, "%s", username);

    GtkWidget *lbl = gtk_label_new("");
    gtk_label_set_markup(GTK_LABEL(lbl), s.bs);

    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);
    set_widget_margins(lbl, 8,8, 5,5);

    StringFree(&s);
    return lbl;
}
static gpointer THREAD_refresh_contacts(gpointer data) {
    return NULL;
}

static void create_invite_ui() {
    clear_controls(G_mainwin);

    // Menu and statusbar
    GtkWidget *menubar = gtk_menu_bar_new();
    GtkWidget *filemenu = gtk_menu_new();
    GtkWidget *filemi = gtk_menu_item_new_with_mnemonic("_File");
    GtkWidget *logoutmi = gtk_menu_item_new_with_mnemonic("_Logout");
    GtkWidget *contactsmi = gtk_menu_item_new_with_mnemonic("_Contacts");
    GtkWidget *quitmi = gtk_menu_item_new_with_mnemonic("_Quit");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(filemi), filemenu);
    gtk_menu_shell_append(GTK_MENU_SHELL(filemenu), logoutmi);
    gtk_menu_shell_append(GTK_MENU_SHELL(filemenu), contactsmi);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), filemi);
    GtkWidget *statusbar = gtk_statusbar_new();

    // Controls
    GtkWidget *lbl = create_label1("Search usernames to invite");
    GtkWidget *txtsearch = gtk_search_entry_new();
    GtkWidget *listresults = gtk_list_box_new();
    GtkWidget *listframe = gtk_frame_new("");
    gtk_container_add(GTK_CONTAINER(listframe), listresults);

    GtkWidget *contentbox = gtk_vbox_new(FALSE, 0);
    set_widget_margins(contentbox, 10, 10, 5, 5);
    gtk_box_pack_start(GTK_BOX(contentbox), lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(contentbox), txtsearch, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(contentbox), listframe, TRUE, TRUE, 0);

    GtkWidget *framebox = gtk_vbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(framebox), menubar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(framebox), contentbox, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(framebox), statusbar, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(G_mainwin), framebox);

    G_ui.statusbar = statusbar;

    G_ui.active_win = INVITEWIN;
    G_ui.invite_txtsearch = txtsearch;

    g_signal_connect(G_OBJECT(quitmi), "activate", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(G_OBJECT(logoutmi), "activate", G_CALLBACK(CALLBACK_menu_logout), NULL);
    g_signal_connect(G_OBJECT(contactsmi), "activate", G_CALLBACK(CALLBACK_menu_contacts), NULL);
    g_signal_connect(G_OBJECT(txtsearch), "activate", G_CALLBACK(CALLBACK_search_users), NULL);

    gtk_widget_set_sensitive(G_mainwin, TRUE);
    gtk_widget_show_all(G_mainwin);
}
static void CALLBACK_search_users(GtkWidget *w, gpointer data) {
    g_thread_new("THREAD_search_users", THREAD_search_users, NULL);
}
static gpointer THREAD_search_users(gpointer data) {
    if (connect_to_server2(G_serverhost, G_serverport, &G_connect_timeout_val, &G_serverfd) == -1)
        return NULL;
    assert(G_serverfd != -1);

    StringAssignFormat(&G_ui.statusbar_text, "Searching...");
    g_idle_add(IDLE_disable_window, NULL);

    Buffer writebuf = BufferNew(256);

    char *searchstr = (char *) gtk_entry_get_text(GTK_ENTRY(G_ui.invite_txtsearch));
    u8 msgno = SEARCHUSERNAME_REQUEST;
    NetPackLen(&writebuf, "%b%s%s", msgno, CSTR(G_session.tok), searchstr);
    send_and_wait_for_response(G_serverfd, &writebuf, &G_timeout_val, on_received_msg);

    BufferFree(&writebuf);
    return NULL;
}

static void CALLBACK_menu_login(GtkWidget *w, gpointer data) {
    create_login_ui("", "", FALSE);
}
static void CALLBACK_menu_logout(GtkWidget *w, gpointer data) {
    remove("cserv_session.txt");
    create_login_ui("", "", FALSE);
}
static void CALLBACK_menu_register(GtkWidget *w, gpointer data) {
    create_register_ui();
}
static void CALLBACK_menu_contacts(GtkWidget *w, gpointer data) {
    create_contacts_ui();
}
static void CALLBACK_menu_invite(GtkWidget *w, gpointer data) {
    create_invite_ui();
}

static gboolean SF_show_connect_error(gpointer data) {
    GtkWidget *dlg = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Error connecting to %s", G_serverhost);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);

    return G_SOURCE_REMOVE;
}

static void on_read_eof() {
    printf("on_read_eof()\n");
}
static void on_server_close() {
    printf("on_server_close()\n");
}
static void on_received_msg(char *msgbytes, u16 len) {
    int z;
    u8 msgno = MSGNO(msgbytes);
    if (msgno == 0)
        return;

    // Skip over msgno (first byte)
    msgbytes++;
    len--;

    if (msgno == LOGINUSER_RESPONSE) {
        LoginUserResponse *resp = malloc0(sizeof(LoginUserResponse));
        resp->msgno = msgno;
        NetUnpack(msgbytes, len, "%s%s%b%s", &resp->tok, &resp->username, &resp->retno, &resp->errortext);
        printf("** LOGINUSER_RESPONSE tok: '%s' username: '%s' retno: %d errortext: '%s' **\n", resp->tok.bs, resp->username.bs, resp->retno, resp->errortext.bs);

        g_idle_add(IDLE_LoginUserResponse, resp);

    } else if (msgno == REGISTERUSER_RESPONSE) {
        RegisterUserResponse *resp = malloc0(sizeof(RegisterUserResponse));
        resp->msgno = msgno;
        NetUnpack(msgbytes, len, "%s%s%b%s", &resp->tok, &resp->username, &resp->retno, &resp->errortext);
        printf("** REGISTERUSER_RESPONSE tok: '%s' username: '%s' retno: %d errortext: '%s' **\n", resp->tok.bs, resp->username.bs, resp->retno, resp->errortext.bs);

        g_idle_add(IDLE_RegisterUserResponse, resp);
    } else if (msgno == SEARCHUSERNAME_RESPONSE) {
        SearchUsernameResponse *resp = malloc0(sizeof(SearchUsernameResponse));
        resp->msgno = msgno;
        NetUnpack(msgbytes, len, "%s", &resp->usernames);
        printf("** SEARCHUSERNAME_RESPONSE usernames: '%s' **\n", CSTR(resp->usernames));

        g_idle_add(IDLE_SearchUsernameResponse, resp);
    }
}

static gboolean IDLE_LoginUserResponse(gpointer data) {
    LoginUserResponse *resp = data;

    StringAssign(&G_session.username, resp->username.bs);
    StringAssign(&G_session.tok, resp->tok.bs);

    if (resp->retno == 0) {
        rename("cserv_session.tmp", "cserv_session.txt");
        set_statusbar(GTK_STATUSBAR(G_ui.statusbar), "Logged on");
        //$$ create_contacts_ui();
        create_invite_ui();
    } else {
        GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(G_mainwin), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s", resp->errortext.bs);
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);

        set_statusbar(GTK_STATUSBAR(G_ui.statusbar), "Login Error");
        gtk_widget_set_sensitive(G_mainwin, TRUE);
    }

    StringFree(&resp->tok);
    StringFree(&resp->errortext);
    free(resp);

    return G_SOURCE_REMOVE;
}

static gboolean IDLE_RegisterUserResponse(gpointer data) {
    RegisterUserResponse *resp = data;

    StringAssign(&G_session.username, resp->username.bs);
    StringAssign(&G_session.tok, resp->tok.bs);

    if (resp->retno == 0) {
        set_statusbar(GTK_STATUSBAR(G_ui.statusbar), "Logged on");
        create_contacts_ui();
    } else {
        GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(G_mainwin), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s", resp->errortext.bs);
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);

        set_statusbar(GTK_STATUSBAR(G_ui.statusbar), "Register Error");
        gtk_widget_set_sensitive(G_mainwin, TRUE);
    }

    StringFree(&resp->tok);
    StringFree(&resp->errortext);
    free(resp);

    return G_SOURCE_REMOVE;
}

static gboolean IDLE_SearchUsernameResponse(gpointer data) {
    SearchUsernameResponse *resp = data;

    gtk_widget_set_sensitive(G_mainwin, TRUE);
    return G_SOURCE_REMOVE;
}

static int connect_to_server(char *serverhost, char *serverport, struct timeval *timeout_val) {
    int z;
    struct addrinfo hints, *ai=NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    StringAssignFormat(&G_ui.statusbar_text, "Connecting to %s...'", serverhost);
    g_idle_add(IDLE_disable_window, NULL);

    // getaddrinfo() will block for some time if an unreachable serverhost (Ex. 'abcdomain') is given.
    z = getaddrinfo0(serverhost, serverport, &hints, &ai);
    if (z != 0) {
        StringAssignFormat(&G_ui.statusbar_text, "Can't reach server '%s'", serverhost);
        g_idle_add(IDLE_enable_window, NULL);

        freeaddrinfo(ai);
        return -1;
    }
    int fd = socket0(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK, ai->ai_protocol);
    if (fd == -1) {
        StringAssignFormat(&G_ui.statusbar_text, "Can't create socket for '%s'", serverhost);
        g_idle_add(IDLE_enable_window, NULL);

        freeaddrinfo(ai);
        return -1;
    }
    int yes=1;
    setsockopt0(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    z = connect0(fd, ai->ai_addr, ai->ai_addrlen);
    freeaddrinfo(ai);
    if (z == 0)
        goto connected;
    if (z < 0 && errno != EINPROGRESS) {
        update_connect_fail_ui();
        CloseSocketFull(fd);
        return -1;
    }
    if (z == -1 && errno == EINPROGRESS) {
        fd_set writefds;
        FD_ZERO(&writefds);
        FD_SET(fd, &writefds);

        while (1) {
            int zz = select(fd+1, NULL, &writefds, NULL, timeout_val);
            if (zz == 0) {
                StringAssignFormat(&G_ui.statusbar_text, "Timeout connecting to '%s'", serverhost);
                g_idle_add(IDLE_enable_window, NULL);

                CloseSocketFull(fd);
                return -1;
            }
            if (zz == -1 && errno == EINTR)
                continue;
            if (zz == -1) {
                fprintf(stderr, "select(): %s\n", strerror(errno));
                update_connect_fail_ui();
                CloseSocketFull(fd);
                return -1;
            }
            assert(zz > 0);
            break;
        }
        assert(FD_ISSET(fd, &writefds));

        int err=0;
        socklen_t errlen = sizeof(err);
        int zz = getsockopt0(fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
        if (zz != 0) {
            fprintf(stderr, "nonblocking connect() error: getsockopt() failed\n");
            update_connect_fail_ui();
            CloseSocketFull(fd);
            return -1;
        } else if (err != 0) {
            fprintf(stderr, "nonblocking connect() error: %s\n", strerror(err));
            update_connect_fail_ui();
            CloseSocketFull(fd);
            return -1;
        }
    }

connected:
    // Socket connected
    StringAssignFormat(&G_ui.statusbar_text, "Connected to %s", serverhost);
    g_idle_add(IDLE_enable_window, NULL);

    return fd;
}
static int connect_to_server2(char *serverhost, char *serverport, struct timeval *timeout_val, int *serverfd) {
    if (*serverfd != -1)
        return 0;
    *serverfd = connect_to_server(serverhost, serverport, timeout_val);
    return *serverfd;
}

static void send_and_wait_for_response(int fd, Buffer *buf, struct timeval *timeout_val, MSGCALLBACK on_recv_msg) {
    int z = NetSend(fd, buf);
    if (z == -1) {
        StringAssignFormat(&G_ui.statusbar_text, "Network error");
        g_idle_add(IDLE_enable_window, NULL);
        return;
    } else if (z == 1) {
        if (wait_for_message_to_be_sent(fd, buf, timeout_val) == -1)
            return;
    }
    wait_for_response_message(fd, timeout_val, on_recv_msg);
}
static int wait_for_message_to_be_sent(int fd, Buffer *writebuf, struct timeval *timeout_val) {
    int z;
    fd_set writefds;
    int maxfd = fd;

    FD_ZERO(&writefds);
    FD_SET(fd, &writefds);

    while (1) {
        z = select(maxfd+1, NULL, &writefds, NULL, timeout_val);
        if (z == 0) {
            StringAssignFormat(&G_ui.statusbar_text, "Timeout during send");
            g_idle_add(IDLE_enable_window, NULL);
            return -1;
        }
        if (z == -1 && errno == EINTR)
            continue;
        if (z == -1) {
            fprintf(stderr, "select(): %s\n", strerror(errno));
            StringAssignFormat(&G_ui.statusbar_text, "Network error");
            g_idle_add(IDLE_enable_window, NULL);
            return -1;
        }
        if (FD_ISSET(fd, &writefds)) {
            z = NetSend(fd, writebuf);
            if (z == 0)
                goto ret;
            if (z == -1) {
                StringAssignFormat(&G_ui.statusbar_text, "Network error");
                g_idle_add(IDLE_enable_window, NULL);
                return -1;
            }
        }
    }
ret:
    g_idle_add(IDLE_enable_window, NULL);
    return 0;
}

static int wait_for_response_message(int fd, struct timeval *timeout_val, MSGCALLBACK on_recv_msg) {
    int z;
    fd_set readfds;
    int maxfd = fd;

    FD_ZERO(&readfds);
    FD_SET(fd, &readfds);

    Buffer readbuf = BufferNew(1024);
    int msglen = 0;

    while (1) {
        z = select(maxfd+1, &readfds, NULL, NULL, timeout_val);
        if (z == 0) {
            StringAssignFormat(&G_ui.statusbar_text, "Timeout while waiting for response");
            g_idle_add(IDLE_enable_window, NULL);
            goto error;
        }
        if (z == -1 && errno == EINTR)
            continue;
        if (z == -1) {
            fprintf(stderr, "select(): %s\n", strerror(errno));
            StringAssignFormat(&G_ui.statusbar_text, "Network error");
            g_idle_add(IDLE_enable_window, NULL);
            goto error;
        }

        if (FD_ISSET(fd, &readfds)) {
            if (NetRecv(fd, &readbuf) == 0)
                goto server_eof;

            // Each message is a 16bit msglen value followed by msglen sequence of bytes.
            // A msglen of 0 means no more bytes remaining in the stream.

            while (1) {
                if (msglen == 0) {
                    if (readbuf.len >= sizeof(u16)) {
                        u16 *bs = (u16 *) readbuf.bs;
                        msglen = ntohs(*bs);
                        if (msglen == 0) {
                            goto server_eof;
                        }
                        BufferShift(&readbuf, sizeof(u16));
                        continue;
                    }
                    break;
                } else {
                    // Read msg body (msglen bytes)
                    if (readbuf.len >= msglen) {
                        if (on_recv_msg)
                            on_recv_msg(readbuf.bs, msglen);
                        BufferShift(&readbuf, msglen);
                        goto success;
                    }
                    break;
                }
            }
        }
    }

server_eof:
    CloseSocketFull(fd);
    StringAssignFormat(&G_ui.statusbar_text, "Server returned no response");
    g_idle_add(IDLE_enable_window, NULL);

error:
    BufferFree(&readbuf);
    return -1;

success:
    BufferFree(&readbuf);
    return 0;
}


