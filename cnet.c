#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <stdarg.h>

#include "clib.h"
#include "cnet.h"

int getaddrinfo0(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res) {
    int z = getaddrinfo(node, service, hints, res);
    if (z != 0)
        fprintf(stderr, "getaddrinfo(): %s\n", gai_strerror(z));
    return z;
}
int socket0(int domain, int type, int protocol) {
    int z = socket(domain, type, protocol);
    if (z == -1)
        fprintf(stderr, "socket(): %s\n", strerror(z));
    return z;
}
int getsockopt0(int sockfd, int level, int optname, void *optval, socklen_t *optlen) {
    int z = getsockopt(sockfd, level, optname, optval, optlen);
    if (z != 0)
        fprintf(stderr, "getsockopt(): %s\n", strerror(z));
    return z;
}
int setsockopt0(int sockfd, int level, int optname, const void *optval, socklen_t optlen) {
    int z = setsockopt(sockfd, level, optname, optval, optlen);
    if (z != 0)
        fprintf(stderr, "setsockopt(): %s\n", strerror(z));
    return z;
}
int connect0(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    int z = connect(sockfd, addr, addrlen);
    if (z != 0 && errno != EINPROGRESS)
        fprintf(stderr, "connect(): %s\n", strerror(z));
    return z;
}

void CloseSocketFull(int fd) {
    shutdown(fd, SHUT_RDWR);
    close(fd);
}
int CreateNonBlockingSocket(char *host, char *port, struct sockaddr *sa) {
    int z;
    struct addrinfo hints, *ai;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    z = getaddrinfo(host, port, &hints, &ai);
    if (z != 0) {
        fprintf(stderr, "getaddrinfo(): %s\n", gai_strerror(z));
        errno = EINVAL;
        return -1;
    }
    if (sa != NULL)
        memcpy(sa, ai->ai_addr, ai->ai_addrlen);

    int fd = socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK, ai->ai_protocol);
    if (fd == -1) {
        fprintf(stderr, "socket(): %s\n", strerror(errno));
        freeaddrinfo(ai);
        return -1;
    }
    int yes=1;
    z = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (z == -1) {
        fprintf(stderr, "setsockopt(): %s\n", strerror(errno));
        freeaddrinfo(ai);
        return -1;
    }

    return fd;
}
int OpenListenSocket(char *host, char *port, int backlog, struct sockaddr *sa) {
    int z;
    struct addrinfo hints, *ai;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    z = getaddrinfo(host, port, &hints, &ai);
    if (z != 0) {
        fprintf(stderr, "getaddrinfo(): %s\n", gai_strerror(z));
        errno = EINVAL;
        return -1;
    }
    if (sa != NULL)
        memcpy(sa, ai->ai_addr, ai->ai_addrlen);

    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd == -1) {
        fprintf(stderr, "socket(): %s\n", strerror(errno));
        freeaddrinfo(ai);
        return -1;
    }
    int yes=1;
    z = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (z == -1) {
        fprintf(stderr, "setsockopt(): %s\n", strerror(errno));
        freeaddrinfo(ai);
        return -1;
    }
    z = bind(fd, ai->ai_addr, ai->ai_addrlen);
    if (z == -1) {
        fprintf(stderr, "bind(): %s\n", strerror(errno));
        freeaddrinfo(ai);
        return -1;
    }
    z = listen(fd, backlog);
    if (z == -1) {
        fprintf(stderr, "listen(): %s\n", strerror(errno));
        freeaddrinfo(ai);
        return -1;
    }
    freeaddrinfo(ai);
    return fd;
}
int OpenConnectSocket(char *host, char *port, int backlog, struct sockaddr *sa) {
    int z;
    struct addrinfo hints, *ai;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    z = getaddrinfo(host, port, &hints, &ai);
    if (z != 0) {
        fprintf(stderr, "getaddrinfo(): %s\n", gai_strerror(z));
        errno = EINVAL;
        return -1;
    }
    if (sa != NULL)
        memcpy(sa, ai->ai_addr, ai->ai_addrlen);

    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd == -1) {
        fprintf(stderr, "socket(): %s\n", strerror(errno));
        freeaddrinfo(ai);
        return -1;
    }
    int yes=1;
    z = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (z == -1) {
        fprintf(stderr, "setsockopt(): %s\n", strerror(errno));
        freeaddrinfo(ai);
        return -1;
    }
    z = connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (z == -1) {
        fprintf(stderr, "connect(): %s\n", strerror(errno));
        freeaddrinfo(ai);
        return -1;
    }
    freeaddrinfo(ai);
    return fd;
}

#define SIN_ADDR(sa) ( (void *) &((struct sockaddr_in *)sa)->sin_addr )
#define SIN6_ADDR(sa) ( (void *) &((struct sockaddr_in6 *)sa)->sin6_addr )
char *GetTextIPAddress(struct sockaddr *sa) {
    static char ipaddr[INET6_ADDRSTRLEN+1];
    void *sin_addr = sa->sa_family == AF_INET ? SIN_ADDR(sa) : SIN6_ADDR(sa);
    if (inet_ntop(sa->sa_family, sin_addr, ipaddr, sizeof(ipaddr)) == NULL) {
        fprintf(stderr, "inet_ntop(): %s\n", strerror(errno));
        return "";
    }
    return ipaddr;
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
                *l = *pbs << 24;
                pbs++;
                if (pbs > maxp) return;
                *l |= *pbs << 16;
                pbs++;
                if (pbs > maxp) return;
                *l |= *pbs << 8;
                pbs++;
                if (pbs > maxp) return;
                *l |= *pbs;
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

