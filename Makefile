VERSION = 0.1

CC	?= clang
LIBS	= `pkg-config --libs x11 xinerama`
CFLAGS	= -DUSE_XINERAMA -DVERSION=\"$(VERSION)\" `pkg-config --cflags x11 xinerama` -g -O0

.PHONY: all clean install

all: mymenu

mymenu: mymenu.c
	$(CC) $(CFLAGS) mymenu.c -o mymenu $(LIBS)

clean:
	rm -f mymenu

install: mymenu
	cp mymenu ~/bin
