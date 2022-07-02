# Copyright (c) 2022 Omar Polo <op@omarpolo.com>
# Copyright (c) 2011, 2013-2022 Ingo Schwarze <schwarze@openbsd.org>
# Copyright (c) 2010, 2011, 2012 Kristaps Dzonsons <kristaps@bsd.lv>
#
# Permission to use, copy, modify, and distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
# WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
# ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
# WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
# ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
# OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

PROG =		mymenu
SRCS =		mymenu.c
OBJS =		${SRCS:.c=.o}
COBJS =		${COBJ:.c=.o}

COMPATSRC =	compat_err.c				\
		compat_getprogname.c			\
		compat_reallocarray.c			\
		compat_recallocarray.c			\
		compat_strtonum.c

TESTSRCS =	test-__progname.c			\
		test-capsicum.c				\
		test-err.c				\
		test-getexecname.c			\
		test-getprogname.c			\
		test-landlock.c				\
		test-pledge.c				\
		test-program_invocation_short_name.c	\
		test-reallocarray.c			\
		test-recallocarray.c			\
		test-static.c				\
		test-strtonum.c

DISTFILES =	LICENSE					\
		Makefile				\
		configure				\
		configure.local.example			\
		mymenu.1				\
		screen-alt.png				\
		screen.png				\
		scripts/mpd.sh				\
		scripts/mru.pl				\
		${SRCS}					\
		${COMPATSRC}				\
		${TESTSRCS}

all: Makefile.configure ${PROG}
.PHONY: clean distclean install uninstall

Makefile.configure config.h: configure ${TESTSRCS}
	@echo "$@ is out of date; please run ./configure"
	@exit 1

include Makefile.configure

${PROG}: ${OBJS} ${COBJS}
	${CC} -o $@ ${OBJS} ${COBJS} ${LDFLAGS} ${LDADD} ${LDADD_LIB_X11}

clean:
	rm -f ${OBJS} ${COBJS} ${PROG}

distclean: clean
	rm -f Makefile.configure config.h config.h.old config.log config.log.old

install:
	mkdir -p ${DESTDIR}${BINDIR}
	mkdir -p ${DESTDIR}${MANDIR}/man1
	${INSTALL_PROGRAM} ${PROG} ${DESTDIR}/${BINDIR}
	${INSTALL_MAN} mymenu.1 ${DESTDIR}${MANDIR}/man1

install-local:
	mkdir -p ${HOME}/bin
	${INSTALL_PROGRAM} ${PROG} ${HOME}/bin/

uninstall:
	rm ${DESTDIR}${BINDIR}/${PROG}
	rm ${DESTDIR}${MANDIR}/man1/mymenu.1

# --- maintainer targets ---

dist: mymenu-${VERSION}.sha256

mymenu-${VERSION}.sha256: mymenu-${VERSION}.tar.gz
	sha256 mymenu-${VERSION}.tar.gz > $@

mymenu-${VERSION}.tar.gz: ${DISTFILES}
	mkdir -p .dist/mymenu-${VERSION}/
	${INSTALL} -m 0644 ${DISTFILES} .dist/mymenu-${VERSION}
	chmod 755 .dist/mymenu-${VERSION}/configure
	(cd .dist/ && tar zcf ../$@ mymenu-${VERSION})
	rm -rf .dist/

mymenu.1.md: mymenu.1
	mandoc -T markdown mymenu.1 > mymenu.1.md
