PROG =		mymenu
SRCS =		mymenu.c
OBJS =		${SRCS:.c=.o}
COBJS =		${COBJ:.c=.o}

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

all: ${PROG}
.PHONY: clean distclean install uninstall

Makefile.configure config.h: configure ${TESTSRCS}
	@echo "$@ is out of date; please run ./configure"
	@exit 1

include Makefile.configure

${PROG}: ${OBJS} ${COBJS}
	${CC} -o $@ ${OBJS} ${COBJS} ${LDFLAGS} ${LDADD} ${LDADD_LIB_X11}

clean:
	rm -f ${OBJS} ${COBS} ${PROG}

install:
	mkdir -p ${DESTDIR}${BINDIR}
	mkdir -p ${DESTDIR}${MANDIR}/man1
	${INSTALL_PROGRAM} ${PROG} ${DESTDIR}/${BINDIR}
	${INSTALL_MAN} mymenu.1 ${DESTDIR}${MANDIR}/man1

uninstall:
	rm ${DESTDIR}${BINDIR}/${PROG}
	rm ${DESTDIR}${MANDIR}/man1/mymenu.1

mymenu.1.md: mymenu.1
	mandoc -T markdown mymenu.1 > mymenu.1.md
