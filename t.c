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
#include "tmcommon.h"

int main(int argc, char *argv[]) {
    Peer p1;
    Peer *p2;

    printf("sizeof(p1.alias) = %ld\n", sizeof(p1.alias));
    printf("sizeof(p2->alias) = %ld\n", sizeof(p2->alias));

    return 0;
}

