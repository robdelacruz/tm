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

int OpenTcpSocket(char *domain, char *port) {
    int z;
    struct addrinfo *hostai;
    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (domain == NULL)
        hints.ai_flags = AI_PASSIVE;

    z = getaddrinfo(domain, port, &hints, &hostai);
    if (z != 0) {
        fprintf(stderr, "getaddrinfo(): %s\n", gai_strerror(z));
        return -1;
    }
    int fd = socket(hostai->ai_family, hostai->ai_socktype, hostai->ai_protocol);
    if (fd == -1) {
        fprintf(stderr, "socket(): %s\n", strerror(errno));
        return -1;
    }
    int yes=1;
    z = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (z == -1) {
        fprintf(stderr, "setsockopt(): %s\n", strerror(errno));
        freeaddrinfo(hostai);
        return -1;
    }
    z = bind(fd, hostai->ai_addr, hostai->ai_addrlen);
    if (z == -1) {
        fprintf(stderr, "OpenTcpSocket bind(): %s\n", strerror(errno));
        freeaddrinfo(hostai);
        return -1;
    }
    printf("OpenTcpSocket() binded to %s\n", SockAddr_ipaddress(hostai->ai_addr));
    freeaddrinfo(hostai);

    return fd;
}

int OpenTcpConnectSocket(char *bindhost, char *bindport, char *connecthost, char *connectport, struct timeval *timeout) {
    int z;
    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    struct addrinfo *bindai;
    z = getaddrinfo(bindhost, bindport, &hints, &bindai);
    if (z != 0) {
        fprintf(stderr, "getaddrinfo() %s/%s : %s\n", bindhost, bindport, gai_strerror(z));
        return -1;
    }
    int fd = socket(bindai->ai_family, bindai->ai_socktype | SOCK_NONBLOCK, bindai->ai_protocol);
    if (fd == -1) {
        fprintf(stderr, "socket(): %s\n", strerror(errno));
        return -1;
    }
    int yes=1;
    z = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (z == -1) {
        fprintf(stderr, "setsockopt(): %s\n", strerror(errno));
        freeaddrinfo(bindai);
        close(fd);
        return -1;
    }
    z = bind(fd, bindai->ai_addr, bindai->ai_addrlen);
    if (z == -1) {
        fprintf(stderr, "bind() %s/%s: %s\n", bindhost, bindport, strerror(errno));
        freeaddrinfo(bindai);
        close(fd);
        return -1;
    }
    freeaddrinfo(bindai);

    struct addrinfo *connectai;
    z = getaddrinfo(connecthost, connectport, &hints, &connectai);
    if (z != 0) {
        fprintf(stderr, "getaddrinfo() %s/%s : %s\n", connecthost, connectport, gai_strerror(z));
        return -1;
    }
    z = connect(fd, connectai->ai_addr, connectai->ai_addrlen);
    freeaddrinfo(connectai);
    if (z == 0)
        goto connected;
    if (z == -1 && errno != EINPROGRESS) {
        fprintf(stderr, "nonblocking connect() %s/%s: %s\n", connecthost, connectport, strerror(errno));
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
                fprintf(stderr, "nonblocking connect() timeout %s/%s\n", connecthost, connectport);
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
                u64 ll = va_arg(args, u32);
                BufferAppendChar(buf, ll >> 56);
                BufferAppendChar(buf, ll >> 48);
                BufferAppendChar(buf, ll >> 40);
                BufferAppendChar(buf, ll >> 32);
                BufferAppendChar(buf, ll >> 24);
                BufferAppendChar(buf, ll >> 16);
                BufferAppendChar(buf, ll >> 8);
                BufferAppendChar(buf, ll);
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

// Like NetPack() but prefixes the block length (u16) at the start of the block.
int NetPackLen(Buffer *buf, char *fmt, ...) {
    // Add 0 block length first, this will be overwritten later.
    u16 msglen = 0;
    BufferAppend(buf, (char *) &msglen, sizeof(msglen));

    va_list args;
    va_start(args, fmt);

    // Add the body
    msglen = NetPackV(buf, fmt, args);

    // Fill in the block length value at the start of the block.
    u16 *pmsglen = (u16 *) (buf->bs + buf->len - msglen - sizeof(msglen));
    *pmsglen = htons(msglen);

    va_end(args);

    return msglen + sizeof(msglen);
}

// NetUnpack(buf, "BUF %b %w %l %s", &n1, &n2, &n3, s1);
void NetUnpack(char *bs, int bslen, char *fmt, ...) {
    int state = 0;  // 0: none, 1: prev '%'
    va_list args;

    va_start(args, fmt);
    char *pbs = bs;
    char *maxp = bs + bslen - 1;
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
                StringAssignFromBytes(str, pbs, slen);
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

