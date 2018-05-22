VERSION = 0.1

# you may want to change these
OPTIONAL = xinerama xft
CDEFS    = -DUSE_XINERAMA -DUSE_XFT -DUSE_STRCASESTR

# you may not want to change these
CC	 ?= cc
LIBS	 = `pkg-config --libs x11 ${OPTIONAL}`
OPTIM    = -O3
CFLAGS 	 = ${CDEFS} -DVERSION=\"$(VERSION)\" `pkg-config --cflags x11 ${OPTIONAL}`

.PHONY: all clean install debug no_xft no_xinerama no_xft_xinerama gnu

all: mymenu

mymenu: mymenu.c
	$(CC) $(CFLAGS) mymenu.c -o mymenu $(LIBS) ${OPTIM}

gnu: mymenu.c
	make CDEFS="-D_GNU_SOURCE ${CDEFS}"

debug:
	make OPTIM="-g -O0 -Wall"

no_xft: mymenu.c
	make OPTIONAL="xinerama" CDEFS="-DUSE_XINERAMA -DUSE_STRCASESTR"

no_xinerama: mymenu.c
	make OPTIONAL="xft" CDEFS="-DUSE_XFT -DUSE_STRCASESTR"

no_xft_xinerama: mymenu.c
	make OPTIONAL="" CDEFS="-DUSE_STRCASESTR"

clean:
	rm -f mymenu

install: mymenu
	cp mymenu ~/bin
