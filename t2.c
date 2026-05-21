#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <stdarg.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "clib.h"
#include "cnet.h"

Arena arena;
Arena scratch;

void printmap(Map m) {
    printf("map len: %d cap: %d\n", m.len, m.cap);
    for (int i=0; i < m.len; i+=2) {
        printf("  %s: '%s'\n", (char *) m.items[i], (char *) m.items[i+1]);
    }
}

void maptest() {
    Map m = MapNew(&arena, 1);
    MapSet(&m, "abc", "Here's a string");
    MapSet(&m, "def", "def string");
    MapSet(&m, "ghi", "ghi string");
    MapSet(&m, "abc", "abc string");
    MapSet(&m, "def", "DEF string");
    printmap(m);

    MapClear(&m);
    printmap(m);
}

void splittest() {
    String s = StringNew(&arena, "abc; def; ghi;  ");
    Array toks = StringSplit(&arena, s, "; ");

    String *ss = (String *) toks.items;
    for (int i=0; i < toks.len; i++)
        printf("[%d] '%s'\n", i, ss[i].bs);
    printf("\n");
}

void string0test() {
    char bytes[] = "abcdefg";
    String s1 = StringNew0(&arena);
    String s2 = StringNew0(&arena);

    printf("s1 a: '%.*s'\n", s1.len, s1.bs);
    printf("s1 b: '%s'\n", CSTR(s1));

    StringAppend(&s2, "abc");
    printf("s2 a: '%.*s'\n", s2.len, s2.bs);
    printf("s2 b: '%s'\n", CSTR(s2));

    s2 = StringNew0(&arena);
    printf("s2 a: '%.*s'\n", s2.len, s2.bs);
    printf("s2 b: '%s'\n", CSTR(s2));

    s2 = StringNew0(&arena);
    printf("s2 a: '%.*s'\n", s2.len, s2.bs);
    printf("s2 b: '%s'\n", CSTR(s2));
    StringAssign(&s2, "abc");
    printf("s2 b: '%s'\n", CSTR(s2));

    s1 = StringNew0(&arena);
    s2 = StringDup(&arena, s1);
    printf("s2 a: '%.*s'\n", s2.len, s2.bs);
    printf("s2 b: '%s'\n", CSTR(s2));
    StringAssign(&s1, "abc");
    s2 = StringDup(&arena, s1);
    StringAssign(&s1, "def");
    printf("s2 a: '%.*s'\n", s2.len, s2.bs);
    printf("s2 b: '%s'\n", CSTR(s2));

    s2 = StringNew0(&arena);
    StringAssignFromBytes(&s2, bytes, sizeof(bytes));
    printf("s2 a: '%.*s'\n", s2.len, s2.bs);

    s2 = StringNew0(&arena);
    StringAssignFormat(&s2, "Hello bytes '%s'\n", bytes);
    printf("s2 a: '%.*s'\n", s2.len, s2.bs);

    s2 = StringNew0(&arena);
    int ifound = StringSearch(s2, 0, "abc");
    printf("ifound: %d\n", ifound);
    ifound = StringSearch(s2, 1, "abc");
    printf("ifound: %d\n", ifound);
    StringAssign(&s2, "ABCabcDEFdef");
    ifound = StringSearch(s2, 0, "abc");
    printf("ifound: %d\n", ifound);
    ifound = StringSearch(s2, 2, "abc");
    printf("ifound: %d\n", ifound);
    ifound = StringSearch(s2, 3, "abc");
    printf("ifound: %d\n", ifound);
    ifound = StringSearch(s2, 4, "abc");
    printf("ifound: %d\n", ifound);

    s2 = StringNew0(&arena);
    int iequals = StringEquals(s2, "a");
    printf("iequals: %d\n", iequals);
    iequals = StringEquals(s2, "");
    printf("iequals: %d\n", iequals);
    s1 = StringNew0(&arena);
    iequals = StringEquals(s2, CSTR(s1));
    printf("iequals: %d\n", iequals);
    StringAssign(&s1, "abc");
    iequals = StringEquals(s2, CSTR(s1));
    printf("iequals: %d\n", iequals);

    s2 = StringNew0(&arena);
    Array ss = StringSplit(&arena, s2, ";");
    printf("ss.len: %d\n", ss.len);
    String *ssitems = (String *) ss.items;
    for (int i=0; i < ss.len; i++)
        printf("ss[%d]: '%s'\n", i, CSTR(ssitems[i]));

    s2 = StringNew0(&arena);
    StringTrim(s2);
    printf("s2 a: '%.*s'\n", s2.len, s2.bs);
    s2 = StringNew(&arena, " ");
    StringTrim(s2);
    printf("s2 b: '%.*s'\n", s2.len, s2.bs);
    s2 = StringNew(&arena, " abc   ");
    StringTrim(s2);
    printf("s2 b: '%.*s'\n", s2.len, s2.bs);
}


int main(int argc, char *argv[]) {
    arena = ArenaNew(255);
    scratch = ArenaNew(255);

    splittest();
    string0test();

    return 0;
}
