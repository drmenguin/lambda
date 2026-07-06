# Copyright (C) 2026 Luke Collins
# SPDX-License-Identifier: GPL-3.0-or-later

CC ?= cc
VERSION ?= 0.1.14
PREFIX ?= /usr
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man
DATADIR ?= $(PREFIX)/share/lambda
INSTALL ?= install
DISTDIR ?= dist
DEBARCH ?= $(shell dpkg --print-architecture 2>/dev/null || echo amd64)
DEBROOT := $(DISTDIR)/lambda_$(VERSION)_$(DEBARCH)

CPPFLAGS ?=
CPPFLAGS += -DLAMBDA_DATADIR=\"$(DATADIR)\"
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
	$(INSTALL) -d "$(DESTDIR)$(DATADIR)"
	$(INSTALL) -m 0644 std.lc "$(DESTDIR)$(DATADIR)/std.lc"

check: all
	./lambda --version
	./lambda-cli --version
	./lambda --define 'I=\x.x' --eval 'I y' | grep -F '[1]>ᵦ y'
	./lambda-cli --eval '(\x.x) \y.y' | grep -F '[1]>ᵦ λy.y'
	./lambda -d 'One=Succ Zero' One | grep -F '  Succ Zero    [One]'
	./lambda --define 'I=\x.x' --free I --eval 'I y' | grep -F 'I y'
	./lambda --eval '(\x y.x y) y' | grep -F '[1]>ᵦ λz.y z'
	./lambda --define 'I=\x.x' --define 'J=\y.y' --eval '\z.z' | grep -F 'λz.z    [I, J]'
	./lambda -d 'Zero=\f x.x' -d 'Succ=\n f x.f (n f x)' -d 'One=Succ Zero' '\f x.f x' | grep -F 'λf x.f x    [One*]'
	./lambda -d 'Zero=\f x.x' -d 'Succ=\n f x.f (n f x)' -d 'One<-Succ Zero' '\f x.f x' | grep -F 'λf x.f x    [One]'
	tmpfile="$$(mktemp)"; \
		printf '%s\n' 'Zero=\f x.x' 'Succ=\n f x.f (n f x)' 'One=Succ Zero' > "$$tmpfile"; \
		./lambda --load "$$tmpfile" '\f x.f x' | grep -F 'λf x.f x    [One*]'; \
		rm -f "$$tmpfile"
	./lambda '(\x.x) y' '%' | grep -F '  y'
	./lambda '(\x.x) y' '%1' | grep -F '  y'
	./lambda '(\x.xx)(\x.xy)z /' '/2' | grep -F '[2]>ᵦ y y z'
	./lambda '\%.%' 2>&1 | grep -F "reserved for history references"
	./lambda '(\x.x) y' --define 'Saved=%' Saved | grep -F '  y    [Saved]'
	./lambda '(\x.x) y' --define 'Saved=%1' Saved | grep -F '  y    [Saved]'
	./lambda '(\x.x) y' --define 'Saved<-%' Saved | grep -F '  y    [Saved]'
	./lambda --load std 'I y' | grep -F '[1]>ᵦ y'
	./lambda --max-steps 1 '(\x.x x) (\x.x x)' | grep -F 'Stopped after 1 steps'
	./lambda-cli --eval '(\x.x) y' | grep -F '[1]>ᵦ y'
	./lambda-cli '(\x.x) y' '%1' | grep -F '[2]> y'
	./lambda-cli '(\x.xx)(\x.xy)z /' '/2' | grep -F '[2]>ᵦ y y z'
	printf '%s\n' '(\x.xx)(\x.xy)z /' '' '' '' | ./lambda-cli | grep -F 'Already in normal form.'
	./lambda-cli '\%.%' 2>&1 | grep -F "reserved for history references"
	./lambda-cli --max-steps 1 '(\x.x x) (\x.x x)' | grep -F 'Stopped after 1 steps'
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
