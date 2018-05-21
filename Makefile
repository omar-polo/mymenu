VERSION = 0.1

CC	 ?= clang
OPTIONAL = xinerama xft
LIBS	 = `pkg-config --libs x11 ${OPTIONAL}`
CFLAGS 	 = -DUSE_XINERAMA -DUSE_XFT -DVERSION=\"$(VERSION)\" `pkg-config --cflags x11 ${OPTIONAL}` -g -O0

.PHONY: all clean install

all: mymenu

mymenu: mymenu.c
	$(CC) $(CFLAGS) mymenu.c -o mymenu $(LIBS)

clean:
	rm -f mymenu

install: mymenu
	cp mymenu ~/bin
