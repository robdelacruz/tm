#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <signal.h>
#include <limits.h>
#include <crypt.h>
#include <time.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "clib.h"
#include "cnet.h"

#define SIN_ADDR(sa) ( (void *) &((struct sockaddr_in *)sa)->sin_addr )
#define SIN6_ADDR(sa) ( (void *) &((struct sockaddr_in6 *)sa)->sin6_addr )

#define BIND_PORT "8003"
#define UDP_LISTEN_PORT "8001"

#define MSGNO(bs) (*((u8 *)bs))
#define PING 1

// PING
typedef struct {
    u8 msgno;
    String alias;
    String hostname;
    String pingtext;
} PingMsg;

void broadcast_whosthere(Arena scratch);
String GetSignature(Arena *arena, Arena scratch, String alias, String hostname);

char *GBindPort = BIND_PORT;
Arena GArena;
Arena GScratch;
String GAlias;
String GHostname;
String GSignature;

int main(int argc, char *argv[]) {
    GArena = ArenaNew(64*1024);
    GScratch = ArenaNew(4096);

    if (argc > 1)
        GBindPort = argv[1];

    char buf[HOST_NAME_MAX];
    int z = gethostname(buf, sizeof(buf));
    if (z == -1) {
        fprintf(stderr, "gethostname() %s\n", strerror(errno));
        exit(1);
    }
    buf[HOST_NAME_MAX-1] = 0;
    GHostname = StringNew(&GArena, buf);

    char *alias = getlogin();
    if (alias == NULL)
        alias = "noname";
    GAlias = StringNew(&GArena, alias);
    StringAssign(&GAlias, "rob");

    GSignature = GetSignature(&GArena, GScratch, GAlias, GHostname);
    printf("GSignature: '%s'\n", CSTR(GSignature));

//    printf("Broadcasting whosthere message.\n");
//    broadcast_whosthere(GScratch);

    printf("Send hello message to localhost.\n");
    int destfd = OpenConnectSocket("localhost", "8002", 12, NULL);
    Buffer sendbuf = BufferNew(&GScratch, 128); 
    NetPackLen(&sendbuf, "%b%s%s%s%s", PING, CSTR(GSignature), CSTR(GAlias), CSTR(GHostname), "hello");
    z = send(destfd, sendbuf.bs, sendbuf.len, 0);
    assert(z == sendbuf.len);

    return 0;
}

void broadcast_whosthere(Arena scratch) {
    int z;
    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;

    struct addrinfo *hostai;
    z = getaddrinfo(NULL, GBindPort, &hints, &hostai);
    if (z != 0) {
        fprintf(stderr, "getaddrinfo(): %s\n", gai_strerror(z));
        exit(1);
    }

    int hostfd = socket(hostai->ai_family, hostai->ai_socktype, hostai->ai_protocol);
    if (hostfd == -1) {
        fprintf(stderr, "socket(): %s\n", strerror(errno));
        exit(1);
    }
    z = bind(hostfd, hostai->ai_addr, hostai->ai_addrlen);
    if (z == -1) {
        fprintf(stderr, "bind(): %s\n", strerror(errno));
        exit(1);
    }
    freeaddrinfo(hostai);

    int yes=1;
    z = setsockopt(hostfd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    if (z == -1) {
        fprintf(stderr, "setsockopt(SO_BROADCAST): %s\n", strerror(errno));
        exit(1);
    }

    struct addrinfo *broadcastai;
    z = getaddrinfo("255.255.255.255", UDP_LISTEN_PORT, &hints, &broadcastai);
    if (z != 0) {
        fprintf(stderr, "getaddrinfo(): %s\n", gai_strerror(z));
        exit(1);
    }

    Buffer buf = BufferNew(&scratch, 32);
    NetPack(&buf, "%b%s%s%s%s", PING, CSTR(GSignature), CSTR(GAlias), CSTR(GHostname), "whosthere");
    z = sendto(hostfd, buf.bs, buf.len, 0, broadcastai->ai_addr, sizeof(struct sockaddr));
    if (z == -1) {
        fprintf(stderr, "broadcast_whosthere() sendto(): %s\n", strerror(errno));
        exit(1);
    }

    freeaddrinfo(broadcastai);

    close(hostfd);
}

String GetSignature(Arena *arena, Arena scratch, String alias, String hostname) {
    static char *CRYPTSALT = "salt1234567890";
    srand(time(NULL));
    String phrase = StringFormat(&scratch, "%s%s%d", CSTR(GAlias), CSTR(GHostname), rand());
    if (phrase.len > CRYPT_MAX_PASSPHRASE_SIZE)
        phrase.bs[CRYPT_MAX_PASSPHRASE_SIZE] = 0;

    struct crypt_data data;
    memset(&data, 0, sizeof(data));
    char *pz = crypt_r(CSTR(phrase), CRYPTSALT, &data);
    assert(pz != NULL);

    return StringNew(arena, data.output);
}

