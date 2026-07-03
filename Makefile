# Copyright (C) 2026 Luke Collins
# SPDX-License-Identifier: GPL-3.0-or-later

CC ?= cc
VERSION ?= 0.1.2
PREFIX ?= /usr
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man
INSTALL ?= install
DISTDIR ?= dist
DEBARCH ?= $(shell dpkg --print-architecture 2>/dev/null || echo amd64)
DEBROOT := $(DISTDIR)/lambda_$(VERSION)_$(DEBARCH)

CPPFLAGS ?=
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -pedantic
LDFLAGS ?=
LDLIBS_NCURSES ?= -lncursesw

.PHONY: all install check deb clean

all: lambda lambda-cli

lambda: main_ncurses.o lambda.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ main_ncurses.o lambda.o $(LDLIBS_NCURSES)

lambda-cli: main_plain.o lambda.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ main_plain.o lambda.o

%.o: %.c lambda.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

install: all
	$(INSTALL) -d "$(DESTDIR)$(BINDIR)"
	$(INSTALL) -m 0755 lambda "$(DESTDIR)$(BINDIR)/lambda"
	$(INSTALL) -m 0755 lambda-cli "$(DESTDIR)$(BINDIR)/lambda-cli"
	$(INSTALL) -d "$(DESTDIR)$(MANDIR)/man1"
	$(INSTALL) -m 0644 lambda.1 "$(DESTDIR)$(MANDIR)/man1/lambda.1"
	$(INSTALL) -m 0644 lambda-cli.1 "$(DESTDIR)$(MANDIR)/man1/lambda-cli.1"

check: all
	./lambda --version
	./lambda-cli --version
	./lambda --define 'I=\x.x' --eval 'I y' | grep -F '→ y'
	./lambda --define 'I=\x.x' --free I --eval 'I y' | grep -F 'I y'
	./lambda --eval '(\x y.x y) y' | grep -F 'λz.y z'
	./lambda --define 'I=\x.x' --define 'J=\y.y' --eval '\z.z' | grep -F 'λz.z    [I, J]'
	./lambda-cli --eval '(\x.x) y' | grep -F '→ y'
	groff -man -Tutf8 lambda.1 >/dev/null
	groff -man -Tutf8 lambda-cli.1 >/dev/null

deb: all
	@if ! command -v dpkg-deb >/dev/null 2>&1; then \
		echo "dpkg-deb is required to build Debian packages"; \
		exit 1; \
	fi
	rm -rf "$(DEBROOT)"
	$(MAKE) DESTDIR="$(CURDIR)/$(DEBROOT)" PREFIX=/usr install
	$(INSTALL) -d "$(DEBROOT)/DEBIAN"
	printf '%s\n' \
		'Package: lambda' \
		'Version: $(VERSION)' \
		'Section: science' \
		'Priority: optional' \
		'Architecture: $(DEBARCH)' \
		'Maintainer: Luke Collins <luke@collins.mt>' \
		'Depends: libc6, libncursesw6' \
		'Homepage: https://github.com/drmenguin/lambda' \
		'Description: Lambda calculus beta-reduction playground' \
		' lambda includes an interactive ncurses interface and a plain command-line reducer.' \
		> "$(DEBROOT)/DEBIAN/control"
	dpkg-deb --build --root-owner-group "$(DEBROOT)" "$(DISTDIR)/lambda_$(VERSION)_$(DEBARCH).deb"

clean:
	rm -f lambda lambda-cli lambda-ncurses *.o
