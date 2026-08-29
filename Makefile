PREFIX ?= /usr/local
PDP10_PREFIX ?= $(PREFIX)
BINDIR ?= $(PDP10_PREFIX)/bin
DATADIR ?= $(PDP10_PREFIX)/share/pdp10-tools
INCLUDEDIR ?= $(PDP10_PREFIX)/include

INSTALL ?= install
LN_S ?= ln -sf
RM ?= rm -f
CC ?= cc
CFLAGS ?= -Wall -Wextra -O2 -std=c99

CTOOLS = mkdsk mkd6fs mkdt mkstream mktap words2pt dlink darc p10run pdp10-objdump mkinitfs0
ALIASES = pdp10-dec-none-darc pdp10-dec-none-objdump mkinitfs
SIMH_INIS = simh/pdp6.ini simh/pdp10-ka.ini simh/pdp10-ki.ini \
	simh/pdp10-kl.ini simh/pdp10-ks.ini
SIMH_NAMES = pdp6.ini pdp10-ka.ini pdp10-ki.ini pdp10-kl.ini pdp10-ks.ini

.PHONY: all clean install uninstall help

all: $(CTOOLS) $(ALIASES)

mkdsk: mkdsk.c
	$(CC) $(CFLAGS) -o $@ mkdsk.c

mkd6fs: mkd6fs.c
	$(CC) $(CFLAGS) -o $@ mkd6fs.c

mkdt: mkdt.c
	$(CC) $(CFLAGS) -o $@ mkdt.c

mkstream: mkstream.c
	$(CC) $(CFLAGS) -o $@ mkstream.c

mktap: mktap.c
	$(CC) $(CFLAGS) -o $@ mktap.c

words2pt: words2pt.c
	$(CC) $(CFLAGS) -o $@ words2pt.c

dlink: dlink.c dobj.c dobj.h
	$(CC) $(CFLAGS) -o $@ dlink.c dobj.c

darc: darc.c dobj.c dobj.h
	$(CC) $(CFLAGS) -o $@ darc.c dobj.c

p10run: p10run.c
	$(CC) $(CFLAGS) -o $@ p10run.c

pdp10-objdump: pdp10-objdump.c dobj.c dobj.h
	$(CC) $(CFLAGS) -o $@ pdp10-objdump.c dobj.c

mkinitfs0: mkinitfs0.c
	$(CC) $(CFLAGS) -o $@ mkinitfs0.c

pdp10-dec-none-darc: darc
	$(LN_S) darc $@

pdp10-dec-none-objdump: pdp10-objdump
	$(LN_S) pdp10-objdump $@

mkinitfs: mkinitfs0
	$(LN_S) mkinitfs0 $@

test: dlink darc p10run pdp10-objdump mktap mkinitfs mkdsk
	$(CC) $(CFLAGS) -std=c89 -I. -o tests/dobj-test tests/dobj-test.c dobj.c
	./tests/dobj-test
	$(CC) $(CFLAGS) -I. -o tests/pdp10-objdump-mk tests/pdp10-objdump-test.c dobj.c
	./tests/pdp10-objdump-test.sh
	./tests/p10run-c89-test.sh
	./tests/p10run-functional-test.sh
	./tests/mktap-mtc-7track-v1-test.sh
	./tests/mkinitfs0-test.sh
	./tests/mkinitfs0-dxr-v1-test.sh
	TMPDIR='$(TMPDIR)' ./tests/mkdsk-member-sectors-v1-test.sh

clean:
	$(RM) $(CTOOLS) $(ALIASES) *.o tests/dobj-test tests/pdp10-objdump-mk tests/*.dobj tests/*.darc tests/*.dxr tests/*.map

install: all
	$(INSTALL) -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(DATADIR)/simh $(DESTDIR)$(INCLUDEDIR)
	$(INSTALL) -m 755 $(CTOOLS) $(DESTDIR)$(BINDIR)/
	$(LN_S) darc $(DESTDIR)$(BINDIR)/pdp10-dec-none-darc
	$(LN_S) pdp10-objdump $(DESTDIR)$(BINDIR)/pdp10-dec-none-objdump
	$(LN_S) mkinitfs0 $(DESTDIR)$(BINDIR)/mkinitfs
	$(INSTALL) -m 644 $(SIMH_INIS) $(DESTDIR)$(DATADIR)/simh/
	$(INSTALL) -m 644 pdp10-sixbit.h $(DESTDIR)$(INCLUDEDIR)/
	@echo "Installed PDP-10 compatibility tools to $(DESTDIR)$(BINDIR)"

uninstall:
	@for f in $(CTOOLS) $(ALIASES); do \
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
