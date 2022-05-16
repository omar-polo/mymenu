.include <bsd.xconf.mk>

PROG =	mymenu

.include "mymenu-version.mk"

CPPFLAGS +=	-I${X11BASE}/include -I${X11BASE}/include/freetype2 -DVERSION=\"${MYMENU_VERSION}\"
LDADD =		-L${X11BASE}/lib -lX11 -lXinerama -lXft

.if "${MYMENU_RELEASE}" == "Yes"
PREFIX ?= /usr/local
BINDIR ?= ${PREFIX}/bin
MANDIR ?= ${PREFIX}/man/man
.else
NOMAN = Yes
CFLAGS += -Werror -Wall -Wstrict-prototypes -Wunused-variable
PREFIX ?= ${HOME}
BINDIR ?= ${PREFIX}/bin
BINOWN = ${USER}
BINGRP != id -g -n
DEBUG = -O0 -g
.endif

release: clean
	sed -i -e 's/_RELEASE=No/_RELEASE=Yes/' mymenu-version.mk
	${MAKE} dist
	sed -i -e 's/_RELEASE=Yes/_RELEASE=No/' mymenu-version.mk

dist: clean
	find . -type -d -name obj -delete
	mkdir /tmp/mymenu-${MYMENU_VERSION}
	pax -rw * /tmp/mymenu-${MYMENU_VERSION}
	rm /tmp/mymenu-${MYMENU_VERSION}/mymenu-dist.txt
	tar -C /tmp -zcf mymenu-${MYMENU_VERSION}.tar.gz mymenu-${MYMENU_VERSION}
	rm -rf /tmp/mymenu-${MYMENU_VERSION}
	tar -ztf mymenu-${MYMENU_VERSION}.tar.gz |
		sed -e 's/^mymenu-${MYMENU_VERSION}//' |
		sort > mymenu-dist.txt.new
	diff -u mymenu-dist.txt{,.new}
	rm mymenu-dist.txt.new

mymenu.1.md: mymenu.1
	mandoc -T markdown mymenu.1 > mymenu.1.md

.include <bsd.prog.mk>
