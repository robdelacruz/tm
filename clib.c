#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <limits.h>
#include <assert.h>
#include <ctype.h>

#include "clib.h"

void panic(char *s) {
    fprintf(stderr, "%s\n", s);
    abort();
}

void *malloc0(size_t size) {
    void *p = malloc(size);
    memset(p, 0, size);
    return p;
}
void *realloc0(void *ptr, size_t oldsize, size_t newsize) {
    assert(newsize > oldsize);
    void *p = realloc(ptr, newsize);
    memset(p + oldsize, 0, newsize - oldsize);
    return p;
}

#define HASHPRIME 31
u16 hash16(char *k, int size) {
    u16 hash=0;
    char ch;
    if (size == 0)
        size = USHRT_MAX+1;

    while (1) {
        char ch = *k;
        if (ch == 0)
            break;
        hash = (hash*HASHPRIME + ch) % size;
        k++;
    }
    return hash;
}

Arena ArenaNew(u32 cap) {
    Arena a;
    if (cap == 0)
        cap = 1024;
    a.bs = malloc0(cap);
    a.pos = 0;
    a.cap = cap;
    return a;
}
Arena ArenaNewAuto(u8 *bytes, u32 bytes_size) {
    Arena a;
    a.bs = bytes;
    a.pos = 0;
    a.cap = bytes_size;
    return a;
}
void ArenaFree(Arena *a) {
    free(a->bs);
    memset(a, 0, sizeof(Arena));
}
void ArenaReset(Arena *a) {
    a->pos = 0;
}
void *ArenaAlloc(Arena *a, u32 size) {
    if (a->pos + size > a->cap) {
        fprintf(stderr, "ArenaAlloc() size: %ld not enough memory\n", size);
        abort();
    }
    u8 *p = a->bs + a->pos;
    a->pos += size;
    return p;
}
void *ArenaRealloc(Arena *a, void *oldbs, u32 oldsize, u32 newsize) {
    void *newbs = ArenaAlloc(a, newsize);
    memcpy(newbs, oldbs, oldsize);
    return newbs;
}
void *ArenaPushBytes(Arena *a, void *src, u32 size) {
    if (a->pos + size > a->cap) {
        fprintf(stderr, "ArenaAlloc() size: %ld not enough memory\n", size);
        abort();
    }
    u8 *p = a->bs + a->pos;
    memcpy(p, src, size);
    a->pos += size;
    return p;
}
void ArenaGet(Arena *a, void *dest, u32 offset, u32 size) {
    if (offset+size > a->pos) {
        fprintf(stderr, "ArenaGet() offset: %ld size: %ld pos: %ld out of bounds\n", offset, size, a->pos);
        memset(dest, 0, size);
        return;
    }
    memcpy(dest, a->bs+offset, size);
}

String StringNew0(Arena *arena) {
    String str = {0};
    str.arena = arena;
    return str;
}
String StringNew(Arena *arena, char *s) {
    String str;
    str.arena = arena;
    str.len = strlen(s);
    str.bs = (char *) ArenaAlloc(arena, str.len+1);
    memcpy(str.bs, s, str.len);    
    str.bs[str.len] = 0;
    return str;
}
String StringNewFromBytes(Arena *arena, char *bs, int bslen) {
    String str;
    str.arena = arena;
    str.bs = (char *) ArenaAlloc(arena, bslen+1);
    memcpy(str.bs, bs, bslen);
    str.bs[bslen] = 0;
    str.len = bslen;
    return str;
}
String StringDup(Arena *arena, String src) {
    String str;
    str.arena = arena;
    str.len = src.len;
    str.bs = (char *) ArenaAlloc(str.arena, str.len+1);
    memcpy(str.bs, src.bs, str.len);    
    str.bs[str.len] = 0;
    return str;
}
String StringFormat(Arena *arena, const char *fmt, ...) {
    String str;
    va_list args;

    va_start(args, fmt);
    str.arena = arena;
    str.len = vsnprintf(NULL, 0, fmt, args);
    str.bs = (char *) ArenaAlloc(arena, str.len+1);
    va_end(args);

    va_start(args, fmt);
    vsnprintf(str.bs, str.len+1, fmt, args);
    va_end(args);

    return str;
}
void StringAppend(String *str, char *s) {
    int slen = strlen(s);
    if (str->len + slen + 1 > USHRT_MAX) // Check for str.len overflow
        return;

    assert(str->arena != NULL);
    str->bs = (char *) ArenaRealloc(str->arena, str->bs, str->len, str->len+slen+1);
    memcpy(str->bs + str->len, s, slen);
    str->len += slen;
    str->bs[str->len] = 0;
}
void StringAppendChar(String *str, char ch) {
    char s[2];
    s[0] = ch;
    s[1] = 0;
    StringAppend(str, s);
}
void StringAssign(String *str, char *s) {
    *str = StringNew(str->arena, s);
}
void StringAssignFromBytes(String *str, char *bs, int bslen) {
    *str = StringNewFromBytes(str->arena, bs, bslen);
}
void StringAssignFormat(String *str, const char *fmt, ...) {
    va_list args;

    va_start(args, fmt);
    str->len = vsnprintf(NULL, 0, fmt, args);
    str->bs = (char *) ArenaAlloc(str->arena, str->len+1);
    va_end(args);

    va_start(args, fmt);
    vsnprintf(str->bs, str->len+1, fmt, args);
    va_end(args);
}
int StringSearch(String str, int startpos, char *searchstr) {
    int searchstr_len = strlen(searchstr);
    for (int i=startpos; i < str.len; i++) {
        for (int isearch=0, istr=i; isearch < searchstr_len && istr < str.len; isearch++, istr++) {
            if (str.bs[istr] != searchstr[isearch])
                break;
            if (isearch == searchstr_len-1) // Match found
                return i;
        }
    }
    return -1;
}
int StringEquals(String str, char *s) {
    int slen = strlen(s);
    if (str.len != slen)
        return 0;
    for (int i=0; i < str.len; i++) {
        if (str.bs[i] != s[i])
            return 0;
    }
    return 1;
}
void StringTrim(String str) {
    if (str.len == 0)
        return;

    // set starti to index of first non-whitespace char
    // set endi to index of last non-whitespace char
    int starti=0;
    int endi=str.len-1;
    for (int i=0; i < str.len; i++) {
        if (!isspace(str.bs[i]))
            break;
        starti++;
    }
    for (int i=str.len-1; i >= 0; i--) {
        if (!isspace(str.bs[i]))
            break;
        endi--;
    }
    if (starti > str.len-1) {
        memset(str.bs, 0, str.len);
        str.len = 0;
        return;
    }
    assert(endi >= starti);

    int newlen = endi-starti+1;
    memmove(str.bs, str.bs+starti, newlen);
    memset(str.bs+newlen, 0, str.len-newlen);
    str.len = newlen;
}

// Returns array of tokens.
Array StringSplit(Arena *arena, String str, char *sep) {
    int itokstart=0;
    int toklen=0;
    int sep_len = strlen(sep);

    // ntoks = number of tokens after splitting string
    int ntoks=1;
    while (1) {
        int isep = StringSearch(str, itokstart, sep);
        if (isep == -1)
            break;

        ntoks++;
        itokstart = isep + sep_len;
    }

    Array toks = ArrayNew(arena, ntoks, sizeof(String));
    itokstart = 0;
    while (1) {
        int isep = StringSearch(str, itokstart, sep);
        if (isep == -1)
            toklen = str.len - itokstart;
        else
            toklen = isep - itokstart;

        String tok = StringNewFromBytes(arena, str.bs+itokstart, toklen);
        ArrayAppend(&toks, &tok);

        if (isep == -1)
            break;
        itokstart = isep + sep_len;
    }

    return toks;
}

Buffer BufferNew(Arena *arena, u32 cap) {
    Buffer buf;
    if (cap == 0)
        cap = 1024;
    buf.arena = arena;
    buf.bs = (char *) ArenaAlloc(arena, cap);
    buf.len = 0;
    buf.cap = cap;
    return buf;
}
void BufferClear(Buffer *buf) {
    memset(buf->bs, 0, buf->len);
    buf->len = 0;
}
void BufferAppend(Buffer *buf, char *bs, u32 bslen) {
    assert(buf->len <= buf->cap);

    // If more space needed, keep doubling capacity until there's enough space.
    u32 newcap = buf->cap;
    while (bslen > newcap - buf->len)
        newcap *= 2;
    if (newcap > buf->cap) {
        assert(buf->arena != NULL);
        buf->bs = (char *) ArenaRealloc(buf->arena, buf->bs, buf->cap, newcap);
        buf->cap = newcap;
    }
    assert(bslen <= buf->cap - buf->len);

    memcpy(buf->bs + buf->len, bs, bslen);
    buf->len += bslen;
}
void BufferAppendChar(Buffer *buf, char c) {
    BufferAppend(buf, &c, 1);
}
// Remove first n bytes of buffer
void BufferShift(Buffer *buf, int n) {
    assert(buf->len <= buf->cap);
    if (n > buf->len)
        n = buf->len;

    memcpy(buf->bs,
           buf->bs + n,
           buf->len - n);
    buf->len -= n;
    memset(buf->bs + buf->len, 0, buf->cap - buf->len);
}

Array ArrayNew(Arena *arena, u16 cap, int itemsize) {
    Array a;
    if (cap == 0)
        cap = 32;

    a.arena = arena;
    a.items = ArenaAlloc(arena, itemsize*cap);
    a.itemsize = itemsize;
    a.len = 0;
    a.cap = cap;
    return a;
}
void ArrayClear(Array *a) {
    memset(a->items, 0, a->itemsize*a->len);
    a->len = 0;
}

static void *array_item_ptr(Array *a, int index) {
    return a->items + index*a->itemsize;
}

void ArrayAppend(Array *a, void *item) {
    assert(a->len <= a->cap);

    // Double the capacity if more space needed
    if (a->len == a->cap) {
        a->items = ArenaRealloc(a->arena, a->items, a->itemsize*a->cap, a->itemsize*a->cap*2);
        a->cap *= 2;
    }
    assert(a->len < a->cap);

    memcpy(array_item_ptr(a, a->len), item, a->itemsize);
    a->len++;
}
void ArrayRemove(Array *a, int index) {
    if (index < 0  || index >= a->len)
        return;

    if (a->len > 1)
        memmove(array_item_ptr(a, index), array_item_ptr(a, index+1), (a->len - index) * a->itemsize);
    memset(array_item_ptr(a, a->len-1), 0, a->itemsize);
    a->len--;
}
void ArrayReplace(Array *a, int index, void *item) {
    if (index < 0  || index >= a->len)
        return;

    memcpy(array_item_ptr(a, index), item, a->itemsize);
}
void *ArrayItem(Array a, int index) {
    assert(index >= 0 && index < a.len);
    return array_item_ptr(&a, index);
}

Map MapNew(Arena *arena, u16 cap) {
    Map m;
    if (cap == 0)
        cap = 32;
    cap*=2;
    m.arena = arena;
    m.items = (void **) ArenaAlloc(arena, sizeof(void *)*cap);
    m.len = 0;
    m.cap = cap;
    return m;
}
void MapClear(Map *m) {
    memset(m->items, 0, sizeof(void *)*m->len);
    m->len = 0;
}
void MapSet(Map *m, char *k, void *v) {
    assert(m->len <= m->cap);

    // Overwrite v if k already in map.
    for (int i=0; i < m->len; i+=2) {
        if (strcmp(m->items[i], k) == 0) {
            m->items[i+1] = v;
            return;
        }
    }

    // Double the capacity if more space needed.
    if (m->len == m->cap) {
        m->items = (void **) ArenaRealloc(m->arena, m->items, sizeof(void *)*m->cap, sizeof(void *)*m->cap * 2);
        m->cap *= 2;
    }
    assert(m->len < m->cap);

    //m->items[m->len] = strdup(k);
    char *newk = ArenaPushBytes(m->arena, k, strlen(k)+1);
    memcpy(newk, k, strlen(k)+1);
    m->items[m->len] = newk;
    m->items[m->len+1] = v;
    m->len += 2;
}
void *MapGet(Map m, char *k) {
    for (int i=0; i < m.len; i+=2) {
        if (strcmp(m.items[i], k) == 0)
            return m.items[i+1];
    }
    return NULL;
}
void MapRemove(Map *m, char *k) {
    for (int i=0; i < m->len; i+=2) {
        if (strcmp(m->items[i], k) == 0) {
            // Move last item to the spot where the deleted item is.
            m->items[i] = m->items[m->len-2];
            m->items[i+1] = m->items[m->len-1];

            memset(&m->items[m->len-2], 0, sizeof(void *)*2);
            m->len -= 2;
        }
    }
}

