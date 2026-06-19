#ifndef CLIB_H
#define CLIB_H

#include <stdio.h>

#define countof(v) (sizeof(v) / sizeof((v)[0]))
#define memzero(p, v) (memset(p, 0, sizeof(v)))
#define CAST(v, type) ((type) (v))
#define STRING(p) ((String) {p, strlen(p)})
#define CSTR(str) ((str).bs? (str).bs : "")
#define CSTRP(strp) (strp->bs? strp->bs : "")
#define CSTR_EQUALS(s1, s2) (strcmp(s1, s2) == 0)

// Create a fixed len size Buffer.
// Same as regular Buffer, except you can't call BufferAppend() on it.
#define BUFFER(bs, size) ((Buffer) {NULL, bs, 0, size})

typedef char i8;
typedef short i16;
typedef long i32;
typedef long long i64;
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned long u32;
typedef unsigned long long u64;

typedef struct {
    u8 *bs;
    u32 pos;
    u32 cap;
} Arena;

typedef struct {
    Arena *arena;
    char *bs;
    u16 len;
} String;

typedef struct {
    Arena *arena;
    char *bs;
    u32 len;
    u32 cap;
} Buffer;

typedef struct {
    Arena *arena;
    void *items;
    int itemsize;
    u16 len;
    u16 cap;
} Array;

typedef struct {
    Arena *arena;
    void **items;
    u16 len;
    u16 cap;
} Map;

typedef void (*FreeFunc)(void *);

void panic(char *s);
void *malloc0(size_t size);
void *realloc0(void *ptr, size_t oldsize, size_t newsize);

u16 hash16(char *k, int size);

Arena ArenaNew(u32 cap);
void ArenaFree(Arena *a);
void ArenaReset(Arena *a);
void *ArenaAlloc(Arena *a, u32 size);
void *ArenaRealloc(Arena *a, void *oldbs, u32 oldsize, u32 newsize);
void *ArenaPushBytes(Arena *a, void *src, u32 size);
void ArenaGet(Arena *a, void *dest, u32 offset, u32 size);

String StringNew0(Arena *arena);
String StringNew(Arena *arena, char *s);
String StringNewFromBytes(Arena *arena, char *bs, int bslen);
String StringDup(Arena *arena, String src);
String StringFormat(Arena *arena, const char *fmt, ...);
void StringAppend(String *str, char *s);
void StringAppendChar(String *str, char ch);
void StringAssign(String *str, char *s);
void StringAssignFromBytes(String *str, char *bs, int bslen);
void StringAssignFormat(String *str, const char *fmt, ...);
int StringSearch(String str, int startpos, char *searchstr);
int StringEquals(String str, char *s);
Array StringSplit(Arena *arena, String str, char *sep);
void StringTrim(String str);

Buffer BufferNew(Arena *arena, u32 cap);
void BufferClear(Buffer *buf);
void BufferAppend(Buffer *buf, char *bs, u32 bslen);
void BufferAppendChar(Buffer *buf, char c);
void BufferShift(Buffer *buf, int n);

Array ArrayNew(Arena *arena, u16 cap, int itemsize);
void ArrayClear(Array *a);
void ArrayAppend(Array *a, void *item);
void ArrayRemove(Array *a, int index);
void ArrayReplace(Array *a, int index, void *item);
void *ArrayItem(Array a, int index);

Map MapNew(Arena *arena, u16 cap);
void MapClear(Map *m);
void MapSet(Map *m, char *k, void *v);
void *MapGet(Map m, char *k);
void MapRemove(Map *m, char *k);

#endif
