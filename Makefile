#
# main Makefile

DESTDIR	?= /
PREFIX	?= /usr
NROFF	?= nroff

#
# XXXrcd: enable these flags when developing using gcc.  I turn them
#         off by default for portability.

# CFLAGS+= -Wall
# CFLAGS+= -Wstrict-prototypes -Wmissing-prototypes -Wpointer-arith
# CFLAGS+= -Wno-sign-compare -Wno-traditional -Wreturn-type
# CFLAGS+= -Wswitch -Wshadow -Wcast-qual -Wwrite-strings -Wextra
# CFLAGS+= -Wno-unused-parameter -Wsign-compare
# CFLAGS+= -Werror

all: prefork prefork.0

clean:
	rm -f prefork prefork.o prefork.0

install:
	mkdir -p			$(DESTDIR)/$(PREFIX)/libexec
	mkdir -p			$(DESTDIR)/$(PREFIX)/man/man8
	mkdir -p			$(DESTDIR)/$(PREFIX)/man/cat8
	install -c -m755 prefork	$(DESTDIR)/$(PREFIX)/libexec
	install -c -m644 prefork.8	$(DESTDIR)/$(PREFIX)/man/man8/
	install -c -m644 prefork.0	$(DESTDIR)/$(PREFIX)/man/cat8/

prefork.0: prefork.8
	$(NROFF) -mandoc prefork.8 > $@
