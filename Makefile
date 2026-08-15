PREFIX ?= /usr/local
PDP10_PREFIX ?= $(PREFIX)
BINDIR ?= $(PDP10_PREFIX)/bin
DATADIR ?= $(PDP10_PREFIX)/share/pdp10-tools

INSTALL ?= install
LN_S ?= ln -sf
RM ?= rm -f
CC ?= cc
CFLAGS ?= -Wall -Wextra -O2 -std=c99

CTOOLS = mkdsk mkdt mkstream mktap words2pt dlink darc p10run
SCRIPTS =
ALIASES = pdp10-dec-none-darc
REMOVED_ALIASES = pdp10-dec-none-ar pdp10-dec-none-ranlib
REMOVED_TOOLS = dxr2rim mkrim mkrim.py p10bare p10bare.py
SIMH_INIS = simh/pdp6.ini simh/pdp10-ka.ini simh/pdp10-ki.ini \
	simh/pdp10-kl.ini simh/pdp10-ks.ini
SIMH_NAMES = pdp6.ini pdp10-ka.ini pdp10-ki.ini pdp10-kl.ini pdp10-ks.ini

.PHONY: all clean install uninstall help test remove-legacy-aliases

all: remove-legacy-aliases $(CTOOLS) $(ALIASES)

remove-legacy-aliases:
	@for f in $(REMOVED_ALIASES); do \
		test ! -L "$$f" || $(RM) "$$f"; \
	done

mkdsk: mkdsk.c
	$(CC) $(CFLAGS) -o $@ mkdsk.c

mkdt: mkdt.c
	$(CC) $(CFLAGS) -o $@ mkdt.c

mkstream: mkstream.c
	$(CC) $(CFLAGS) -o $@ mkstream.c

mktap: mktap.c
	$(CC) $(CFLAGS) -o $@ mktap.c

words2pt: words2pt.c
	$(CC) $(CFLAGS) -o $@ words2pt.c

dlink: dlink.c dobj.c dobj.h
	$(CC) $(CFLAGS) -std=c89 -o $@ dlink.c dobj.c

darc: darc.c dobj.c dobj.h
	$(CC) $(CFLAGS) -std=c89 -o $@ darc.c dobj.c

p10run: p10run.c
	$(CC) $(CFLAGS) -std=c89 -o $@ p10run.c

pdp10-dec-none-darc: darc
	$(LN_S) darc $@

test: dlink darc p10run
	$(CC) $(CFLAGS) -std=c89 -I. -o tests/dobj-test tests/dobj-test.c dobj.c
	./tests/dobj-test
	./tests/p10run-c89-test.sh
	./tests/p10run-functional-test.sh

clean:
	$(RM) $(CTOOLS) $(ALIASES) $(REMOVED_ALIASES) $(REMOVED_TOOLS) *.o tests/dobj-test tests/*.dobj tests/*.darc tests/*.dxr tests/*.map
	$(RM) -r __pycache__ legacy/__pycache__

install: all
	$(INSTALL) -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(DATADIR)/simh
	@for f in $(REMOVED_TOOLS); do $(RM) "$(DESTDIR)$(BINDIR)/$$f"; done
	$(INSTALL) -m 755 $(CTOOLS) $(SCRIPTS) $(DESTDIR)$(BINDIR)/
	@for f in $(REMOVED_ALIASES); do \
		test ! -L "$(DESTDIR)$(BINDIR)/$$f" || $(RM) "$(DESTDIR)$(BINDIR)/$$f"; \
	done
	$(LN_S) darc $(DESTDIR)$(BINDIR)/pdp10-dec-none-darc
	$(INSTALL) -m 644 $(SIMH_INIS) $(DESTDIR)$(DATADIR)/simh/
	@echo "Installed PDP-10 compatibility tools to $(DESTDIR)$(BINDIR)"

uninstall:
	@for f in $(CTOOLS) $(SCRIPTS) pdp10-dec-none-darc; do \
		$(RM) "$(DESTDIR)$(BINDIR)/$$f"; \
	done
	$(RM) $(ALIASES)
	@for f in $(SIMH_NAMES); do \
		$(RM) "$(DESTDIR)$(DATADIR)/simh/$$f"; \
	done
	@echo "Uninstalled PDP-10 compatibility tools from $(DESTDIR)$(BINDIR)"

help:
	@echo "PDP-10 compatibility tools Makefile"
	@echo ""
	@echo "  make                build host C tools"
	@echo "  make install        install tools and SIMH config data"
	@echo "  make uninstall      remove installed tools and SIMH config data"
	@echo "  make clean          remove generated artifacts"
	@echo ""
	@echo "Variables:"
	@echo "  PREFIX=/usr/local   default installation prefix"
	@echo "  PDP10_PREFIX=...    PDP-10 toolchain prefix; overrides PREFIX"
