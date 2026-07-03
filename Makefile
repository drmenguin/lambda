# Copyright (C) 2026 Luke Collins
# SPDX-License-Identifier: GPL-3.0-or-later

CC ?= cc
PREFIX ?= /usr
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man
INSTALL ?= install

CPPFLAGS ?=
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -pedantic
LDFLAGS ?=
LDLIBS_NCURSES ?= -lncursesw

.PHONY: all install check clean

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
	./lambda-cli --eval '(\x.x) y' | grep -F '→ y'
	groff -man -Tutf8 lambda.1 >/dev/null
	groff -man -Tutf8 lambda-cli.1 >/dev/null

clean:
	rm -f lambda lambda-cli lambda-ncurses *.o
