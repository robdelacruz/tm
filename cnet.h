#ifndef CNET_H
#define CNET_H

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "clib.h"

int getaddrinfo0(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res);
int socket0(int domain, int type, int protocol);
int getsockopt0(int sockfd, int level, int optname, void *optval, socklen_t *optlen);
int setsockopt0(int sockfd, int level, int optname, const void *optval, socklen_t optlen);
int connect0(int sockfd, const struct sockaddr *addr, socklen_t addrlen);

void CloseSocketFull(int fd);
int CreateNonBlockingSocket(char *host, char *port, struct sockaddr *sa);
int OpenListenSocket(char *host, char *port, int backlog, struct sockaddr *sa);
int OpenConnectSocket(char *host, char *port, int backlog, struct sockaddr *sa);
char *GetTextIPAddress(struct sockaddr *sa);
int NetRecv(int fd, Buffer *buf);
int NetSend(int fd, Buffer *buf);
int NetSend2(int fd, Buffer *buf, fd_set *writefds, int *maxfd);
int NetPack(Buffer *buf, char *fmt, ...);
int NetPackLen(Buffer *buf, char *fmt, ...);
void NetUnpack(char *bs, int bslen, char *fmt, ...);

#endif
