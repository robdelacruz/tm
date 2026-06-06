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

// HostAddr combines IPv4 address (sin_addr 32 bits) + network port (sin_port 16 bits)
// hostaddr = (sin_port << 32) + sin_addr
typedef u64 HostAddr;

HostAddr HostAddrFromSockAddr(struct sockaddr_in *sa);
struct sockaddr_in SockAddrFromHostAddr(HostAddr hostaddr);
struct in_addr HostAddr_addr(HostAddr hostaddr);
in_port_t HostAddr_port(HostAddr hostaddr);

char *HostAddr_ipaddress(HostAddr hostaddr);
void HostAddr_ipaddress2(HostAddr hostaddr, String *outstr);

char *SockAddr_ipaddress(struct sockaddr *sa);
void SockAddr_ipaddress2(struct sockaddr *sa, String *outstr);

int getaddrinfo0(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res);
int socket0(int domain, int type, int protocol);
int getsockopt0(int sockfd, int level, int optname, void *optval, socklen_t *optlen);
int setsockopt0(int sockfd, int level, int optname, const void *optval, socklen_t optlen);
int connect0(int sockfd, const struct sockaddr *addr, socklen_t addrlen);

int OpenTcpSocket(char *domain, char *port);
int OpenTcpConnectSocket(char *bindhost, char *bindport, char *connecthost, char *connectport, struct timeval *timeout);
void ShutdownSocket(int fd);
int NetRecv(int fd, Buffer *buf);
int NetSend(int fd, Buffer *buf);
int NetSend2(int fd, Buffer *buf, fd_set *writefds, int *maxfd);
int NetPack(Buffer *buf, char *fmt, ...);
int NetPackLen(Buffer *buf, char *fmt, ...);
void NetUnpack(char *bs, int bslen, char *fmt, ...);

#endif
