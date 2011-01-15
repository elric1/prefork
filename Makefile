#
# main Makefile

PREFIX ?= /usr/local

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
	install -c -m755 prefork	$(DESTDIR)/$(PREFIX)/sbin
	install -c -m644 prefork.1	$(DESTDIR)/$(PREFIX)/man/man1/
	install -c -m644 prefork.0	$(DESTDIR)/$(PREFIX)/man/cat1/

prefork.0: prefork.1
	nroff -mandoc prefork.1 > $@
