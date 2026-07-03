#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <stdarg.h>

#include "clib.h"
#include "cnet.h"

#define U32(n) ((u32) n)
#define U64(n) ((u64) n)

// Conversion from HostAddr <--> sockaddr_in
HostAddr HostAddrFromSockAddr(struct sockaddr_in *sa) {
    return ((u64) sa->sin_port << 32) + sa->sin_addr.s_addr;
}
struct sockaddr_in SockAddrFromHostAddr(HostAddr hostaddr) {
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = HostAddr_port(hostaddr);
    sa.sin_addr = HostAddr_addr(hostaddr);
    return sa;
}
struct in_addr HostAddr_addr(HostAddr hostaddr) {
    struct in_addr sin_addr = {0};
    sin_addr.s_addr = (u32) (hostaddr & 0x00000000FFFFFFFF);
    return sin_addr;
}
in_port_t HostAddr_port(HostAddr hostaddr) {
    return (in_port_t) (hostaddr >> 32);
}

char *HostAddr_ipaddress(HostAddr hostaddr) {
    static char ipaddr[INET6_ADDRSTRLEN+1];
    struct in_addr sin_addr = HostAddr_addr(hostaddr);
    if (inet_ntop(AF_INET, &sin_addr, ipaddr, sizeof(ipaddr)) == NULL) {
        fprintf(stderr, "inet_ntop(): %s\n", strerror(errno));
        return "";
    }
    return ipaddr;
}
void HostAddr_ipaddress2(HostAddr hostaddr, String *outstr) {
    char ipaddr[INET6_ADDRSTRLEN+1];
    struct in_addr sin_addr = HostAddr_addr(hostaddr);
    if (inet_ntop(AF_INET, &sin_addr, ipaddr, sizeof(ipaddr)) == NULL) {
        fprintf(stderr, "inet_ntop(): %s\n", strerror(errno));
        StringAssign(outstr, "");
        return;
    }
    StringAssign(outstr, ipaddr);
}
int HostAddr_ipaddr_equals(HostAddr addr1, HostAddr addr2) {
    if (HostAddr_addr(addr1).s_addr == HostAddr_addr(addr2).s_addr)
        return 1;
    return 0;
}

#define SIN_ADDR(sa) ( (void *) &((struct sockaddr_in *)sa)->sin_addr )
#define SIN6_ADDR(sa) ( (void *) &((struct sockaddr_in6 *)sa)->sin6_addr )
char *SockAddr_ipaddress(struct sockaddr *sa) {
    static char ipaddr[INET6_ADDRSTRLEN+1];
    void *sin_addr = sa->sa_family == AF_INET ? SIN_ADDR(sa) : SIN6_ADDR(sa);
    if (inet_ntop(sa->sa_family, sin_addr, ipaddr, sizeof(ipaddr)) == NULL) {
        fprintf(stderr, "inet_ntop(): %s\n", strerror(errno));
        return "";
    }
    return ipaddr;
}

void SockAddr_ipaddress2(struct sockaddr *sa, String *outstr) {
    char ipaddr[INET6_ADDRSTRLEN+1];
    void *sin_addr = sa->sa_family == AF_INET ? SIN_ADDR(sa) : SIN6_ADDR(sa);
    if (inet_ntop(sa->sa_family, sin_addr, ipaddr, sizeof(ipaddr)) == NULL) {
        fprintf(stderr, "inet_ntop(): %s\n", strerror(errno));
        StringAssign(outstr, "");
        return;
    }
    StringAssign(outstr, ipaddr);
}

int getaddrinfo0(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res) {
    int z = getaddrinfo(node, service, hints, res);
    if (z != 0)
        fprintf(stderr, "getaddrinfo(): %s\n", gai_strerror(z));
    return z;
}
int socket0(int domain, int type, int protocol) {
    int z = socket(domain, type, protocol);
    if (z == -1)
        fprintf(stderr, "socket(): %s\n", strerror(errno));
    return z;
}
int getsockopt0(int sockfd, int level, int optname, void *optval, socklen_t *optlen) {
    int z = getsockopt(sockfd, level, optname, optval, optlen);
    if (z != 0)
        fprintf(stderr, "getsockopt(): %s\n", strerror(errno));
    return z;
}
int setsockopt0(int sockfd, int level, int optname, const void *optval, socklen_t optlen) {
    int z = setsockopt(sockfd, level, optname, optval, optlen);
    if (z != 0)
        fprintf(stderr, "setsockopt(): %s\n", strerror(errno));
    return z;
}
int connect0(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    int z = connect(sockfd, addr, addrlen);
    if (z != 0 && errno != EINPROGRESS)
        fprintf(stderr, "connect(): %s\n", strerror(errno));
    return z;
}

int GetIPV4Address(char *host, int port, SockAddrIPV4 *sa) {
    char portstr[10];
    snprintf(portstr, sizeof(portstr), "%d", port);

    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    struct addrinfo *ai;
    int z = getaddrinfo(host, portstr, &hints, &ai);
    if (z != 0) {
        fprintf(stderr, "getaddrinfo() %s/%s : %s\n", host, portstr, gai_strerror(z));
        return -1;
    }

    assert(ai->ai_family == AF_INET);
    assert(ai->ai_addrlen == sizeof(SockAddrIPV4));
    memcpy(sa, ai->ai_addr, sizeof(SockAddrIPV4));

    freeaddrinfo(ai);
    return 0;
}

int OpenTcpSocket(char *host, int port) {
    int z;
    struct sockaddr_in sa;

    if (GetIPV4Address(host, port, &sa) == -1)
        return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        fprintf(stderr, "socket(): %s\n", strerror(errno));
        return -1;
    }
    int yes=1;
    z = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (z == -1) {
        fprintf(stderr, "setsockopt(): %s\n", strerror(errno));
        return -1;
    }
    z = bind(fd, (struct sockaddr *) &sa, sizeof(sa));
    if (z == -1) {
        fprintf(stderr, "OpenTcpSocket bind(): %s\n", strerror(errno));
        return -1;
    }

    return fd;
}

int OpenTcpConnectSocket(int bindport, struct sockaddr *sa, socklen_t sa_len, struct timeval *timeout) {
    int z;

    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd == -1) {
        fprintf(stderr, "OpenTcpConnectSocket() socket(): %s\n", strerror(errno));
        return -1;
    }
    int yes=1;
    z = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (z == -1) {
        fprintf(stderr, "setsockopt(): %s\n", strerror(errno));
        return -1;
    }

    struct sockaddr_in bindsa;
    memset(&bindsa, 0, sizeof(bindsa));
    bindsa.sin_family = AF_INET;
    bindsa.sin_port = htons(bindport);
    bindsa.sin_addr.s_addr = INADDR_ANY;
    z = bind(fd, (struct sockaddr *) &bindsa, sizeof(bindsa));
    if (z == -1) {
        fprintf(stderr, "OpenTcpConnectSocket() bind(): %s\n", strerror(errno));
        return -1;
    }

    z = connect(fd, sa, sa_len);
    if (z == 0)
        goto connected;
    if (z == -1 && errno != EINPROGRESS) {
        fprintf(stderr, "nonblocking connect(): %s\n", strerror(errno));
        ShutdownSocket(fd);
        return -1;
    }
    if (z == -1 && errno == EINPROGRESS) {
        fd_set writefds;
        FD_ZERO(&writefds);
        FD_SET(fd, &writefds);

        while (1) {
            int zz = select(fd+1, NULL, &writefds, NULL, timeout);
            if (zz == 0) {
                fprintf(stderr, "nonblocking connect() timeout\n");
                ShutdownSocket(fd);
                return -1;
            }
            if (zz == -1 && errno == EINTR)
                continue;
            if (zz == -1) {
                fprintf(stderr, "nonblocking connect select(): %s\n", strerror(errno));
                ShutdownSocket(fd);
                return -1;
            }
            assert(zz > 0);
            break;
        }
        assert(FD_ISSET(fd, &writefds));

        int err=0;
        socklen_t errlen = sizeof(err);
        int zz = getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
        if (zz == -1) {
            fprintf(stderr, "nonblocking connect getsockopt(): %s\n", strerror(errno));
            ShutdownSocket(fd);
            return -1;
        } 
        if (err != 0) {
            fprintf(stderr, "nonblocking connect() error: %s\n", strerror(err));
            ShutdownSocket(fd);
            return -1;
        }
    }

connected:
    return fd;
}
int OpenTcpConnectSocket2(int bindport, HostAddr hostaddr, struct timeval *timeout) {
    SockAddrIPV4 sa = SockAddrFromHostAddr(hostaddr);
    return OpenTcpConnectSocket(bindport, (struct sockaddr *) &sa, sizeof(sa), timeout);
}
int OpenTcpConnectSocket3(int bindport, char *host, int port, struct timeval *timeout) {
    SockAddrIPV4 sa;
    if (GetIPV4Address(host, port, &sa) == -1)
        return -1;
    return OpenTcpConnectSocket(bindport, (struct sockaddr *) &sa, sizeof(sa), timeout);
}

void ShutdownSocket(int fd) {
    shutdown(fd, SHUT_RDWR);
    close(fd);
}

// Nonblocking socket read into buf.
// Returns  0 for EOF (socket was shutdown)
//         -1 if error occured (check errno)
//          1 if socket is still open for receiving data
int NetRecv(int fd, Buffer *buf) {
    int z;
    char readbuf[1024];
    while (1) {
        z = recv(fd, readbuf, sizeof(readbuf), MSG_DONTWAIT);
        if (z == 0)
            return 0;
        if (z == -1 && errno == EINTR)
            continue;
        if (z == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 1;
        if (z == -1) {
            fprintf(stderr, "recv() on socket %d: %s\n", fd, strerror(errno));
            return -1; 
        }
        assert(z > 0);
        BufferAppend(buf, readbuf, z);
    }
    return 1;
}
// Nonblocking socket write from buf.
// Returns  0 for all bytes sent
//         -1 if error occured (check errno)
//          1 for partial bytes sent
// Successfully sent data is removed from buf, unsent data remains in buf.
int NetSend(int fd, Buffer *buf) {
    int z;
    while (1) {
        if (buf->len <= 0)
            return 0;
        z = send(fd, buf->bs, buf->len, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (z == -1 && errno == EINTR)
            continue;
        if (z == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 1;
        if (z == -1) {
            fprintf(stderr, "send() on socket %d: %s\n", fd, strerror(errno));
            return -1; 
        }
        assert(z > 0);
        BufferShift(buf, z);
    }
    return 1;
}

int NetSend2(int fd, Buffer *buf, fd_set *writefds, int *maxfd) {
    int z = NetSend(fd, buf);
    // Can also use  if (buf.len == 0)
    if (z == 0) {
        FD_CLR(fd, writefds);
    } else if (z == 1) {
        FD_SET(fd, writefds);
        if (fd > *maxfd)
            *maxfd = fd;
    }
    return z;
}

int NetSend_wait_until_complete(int fd, Buffer *sendbuf, struct timeval *timeout) {
    int z = NetSend(fd, sendbuf);
    if (z == -1)
        return -1;
    if (z == 0)
        return 0;

    fd_set writefds;
    FD_ZERO(&writefds);
    FD_SET(fd, &writefds);

    while (1) {
        z = select(fd+1, NULL, &writefds, NULL, timeout);
        if (z == 0) {
            fprintf(stderr, "NetSend_wait_until_complete timeout\n");
            return -1;
        }
        if (z == -1 && errno == EINTR)
            continue;
        if (z == -1) {
            fprintf(stderr, "NetSend_wait_until_complete select(): %s\n", strerror(errno));
            return -1;
        }
        if (FD_ISSET(fd, &writefds)) {
            z = NetSend(fd, sendbuf);
            if (z == -1)
                return -1;
            if (z == 0)
                break;
        }
    }
    return 0;
}

int NetPackV(Buffer *buf, char *fmt, va_list args) {
    int state = 0;  // 0: none, 1: prev '%'
    int nbytes_packed = 0;

    for (char *pfmt = fmt; *pfmt != 0; pfmt++) {
        if (state == 0) {
            if (*pfmt == '%') {
                state = 1;
                continue;
            }
            BufferAppendChar(buf, *pfmt);
            nbytes_packed++;
            continue;
        }
        if (state == 1) {
            if (*pfmt == 'b') {
                u8 ch = va_arg(args, int);
                BufferAppendChar(buf, ch);
                nbytes_packed++;
            } else if (*pfmt == 'w') {
                u16 w = va_arg(args, int);
                BufferAppendChar(buf, w >> 8);
                BufferAppendChar(buf, w);
                nbytes_packed += 2;
            } else if (*pfmt == 'l') {
                u32 l = va_arg(args, u32);
                BufferAppendChar(buf, l >> 24);
                BufferAppendChar(buf, l >> 16);
                BufferAppendChar(buf, l >> 8);
                BufferAppendChar(buf, l);
                nbytes_packed += 4;
            } else if (*pfmt == 'L') {
                u64 ll = va_arg(args, u64);
                BufferAppendByte(buf, ll >> 56);
                BufferAppendByte(buf, ll >> 48);
                BufferAppendByte(buf, ll >> 40);
                BufferAppendByte(buf, ll >> 32);
                BufferAppendByte(buf, ll >> 24);
                BufferAppendByte(buf, ll >> 16);
                BufferAppendByte(buf, ll >> 8);
                BufferAppendByte(buf, ll);
                nbytes_packed += 8;
            } else if (*pfmt == 's') {
                char *s = va_arg(args, char *);
                u16 slen = strlen(s);
                BufferAppendChar(buf, slen >> 8);
                BufferAppendChar(buf, slen);
                nbytes_packed += 2;
                BufferAppend(buf, s, slen);
                nbytes_packed += slen;
            } else {
                // Ignore any unsupported %? spec
            }
            state = 0;
            continue;
        }
    }

    return nbytes_packed;
}


// NetPack(buf, "%b%w%l%s", n1, n2, n3, "abc");
// Returns number of bytes packed.
int NetPack(Buffer *buf, char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int nbytes_packed = NetPackV(buf, fmt, args);
    va_end(args);

    return nbytes_packed;
}

int NetPackLenV(Buffer *buf, char *fmt, va_list args) {
    // Add 0 block length first, this will be overwritten later.
    u16 msglen = 0;
    BufferAppend(buf, (char *) &msglen, sizeof(msglen));

    // Add the body
    msglen = NetPackV(buf, fmt, args);

    // Fill in the block length value at the start of the block.
    u16 *pmsglen = (u16 *) (buf->bs + buf->len - msglen - sizeof(msglen));
    *pmsglen = htons(msglen);

    return msglen + sizeof(msglen);
}

// Like NetPack() but prefixes the block length (u16) at the start of the block.
int NetPackLen(Buffer *buf, char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    int len = NetPackLenV(buf, fmt, args);

    va_end(args);
    return len;
}

// NetUnpack(buf, "BUF %b %w %l %s", &n1, &n2, &n3, s1);
void NetUnpack(char *bs, int bslen, char *fmt, ...) {
    int state = 0;  // 0: none, 1: prev '%'
    va_list args;

    va_start(args, fmt);
    u8 *pbs = (u8*)bs;
    u8 *maxp = (u8*)bs + bslen - 1;
    for (char *pfmt = fmt; *pfmt != 0; pfmt++) {
        if (pbs > maxp) return;

        if (state == 0) {
            if (*pfmt == '%') {
                state = 1;
                continue;
            }
            pbs++;
            if (pbs > maxp) return;
            continue;
        }
        if (state == 1) {
            if (*pfmt == 'b') {
                u8 *ch = va_arg(args, u8*);
                *ch = *pbs;
                pbs++;
                if (pbs > maxp) return;
            } else if (*pfmt == 'w') {
                u16 *w = va_arg(args, u16*);
                *w = *pbs << 8;
                pbs++;
                if (pbs > maxp) return;
                *w |= *pbs;
                pbs++;
            } else if (*pfmt == 'l') {
                u32 *l = va_arg(args, u32 *);
                *l = U32(*pbs) << 24;
                pbs++;
                if (pbs > maxp) return;
                *l |= U32(*pbs) << 16;
                pbs++;
                if (pbs > maxp) return;
                *l |= U32(*pbs) << 8;
                pbs++;
                if (pbs > maxp) return;
                *l |= *pbs;
                pbs++;
            } else if (*pfmt == 'L') {
                u64 *ll = va_arg(args, u64 *);
                *ll = U64(*pbs) << 56;
                pbs++;
                if (pbs > maxp) return;
                *ll |= U64(*pbs) << 48;
                pbs++;
                if (pbs > maxp) return;
                *ll |= U64(*pbs) << 40;
                pbs++;
                if (pbs > maxp) return;
                *ll |= U64(*pbs) << 32;
                pbs++;
                if (pbs > maxp) return;
                *ll |= U64(*pbs) << 24;
                pbs++;
                if (pbs > maxp) return;
                *ll |= U64(*pbs) << 16;
                pbs++;
                if (pbs > maxp) return;
                *ll |= U64(*pbs) << 8;
                pbs++;
                if (pbs > maxp) return;
                *ll |= *pbs;
                pbs++;
            } else if (*pfmt == 's') {
                String *str = (String *) va_arg(args, void *);
                u16 slen = *pbs << 8;
                pbs++;
                if (pbs > maxp) return;
                slen |= *pbs;
                pbs++;
                if (pbs > maxp) return;
                StringAssignFromBytes(str, (char*)pbs, slen);
                pbs += slen;
            } else {
                // Ignore any unsupported %? spec
            }
            state = 0;
            continue;
        }
    }
    va_end(args);
}

