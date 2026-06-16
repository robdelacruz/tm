LIBS=
CFLAGS=-std=gnu99 -Wall -Werror
CFLAGS+= -Wno-deprecated-declarations -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
GTK_CFLAGS=`pkg-config --cflags gtk+-3.0`
GTK_LIBS=`pkg-config --libs gtk+-3.0`

all: tm t t2

t: t.c clib.c
	gcc $(CFLAGS) -o $@ $^ $(LIBS)

t2: t2.c clib.c
	gcc $(CFLAGS) -o $@ $^ $(LIBS)

tm: tm.c clib.c cnet.c tmcommon.c
#	gcc $(CFLAGS) $(GTK_CFLAGS) -o $@ $^ $(LIBS) $(GTK_LIBS)
	gcc $(CFLAGS) -o $@ $^ $(LIBS) 

tmtracker: tmtracker.c clib.c cnet.c tmcommon.c
#	gcc $(CFLAGS) $(GTK_CFLAGS) -o $@ $^ $(LIBS) $(GTK_LIBS)
	gcc $(CFLAGS) -o $@ $^ $(LIBS) 

clean:
	rm -rf tm tmtracker t t2

