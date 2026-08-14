PREFIX ?= /usr/local
PDP10_PREFIX ?= $(PREFIX)
BINDIR ?= $(PDP10_PREFIX)/bin
DATADIR ?= $(PDP10_PREFIX)/share/pdp10-tools

INSTALL ?= install
LN_S ?= ln -sf
RM ?= rm -f
CC ?= cc
CFLAGS ?= -Wall -Wextra -O2 -std=c99

CTOOLS = dxr2rim mkdsk mkdt mkrim mkstream mktap words2pt
SCRIPTS = p10bare
ALIASES = pdp10-dec-none-ar pdp10-dec-none-ranlib
TARGET_AR ?= ar
TARGET_RANLIB ?= ranlib
LEGACY = legacy/mkrim.py
SIMH_INIS = simh/pdp6.ini simh/pdp10-ka.ini simh/pdp10-ki.ini \
	simh/pdp10-kl.ini simh/pdp10-ks.ini
SIMH_NAMES = pdp6.ini pdp10-ka.ini pdp10-ki.ini pdp10-kl.ini pdp10-ks.ini

.PHONY: all clean install uninstall help

all: $(CTOOLS) $(ALIASES)

dxr2rim: dxr2rim.c
	$(CC) $(CFLAGS) -o $@ dxr2rim.c

mkdsk: mkdsk.c
	$(CC) $(CFLAGS) -o $@ mkdsk.c

mkdt: mkdt.c
	$(CC) $(CFLAGS) -o $@ mkdt.c

mkrim: mkrim.c
	$(CC) $(CFLAGS) -o $@ mkrim.c

mkstream: mkstream.c
	$(CC) $(CFLAGS) -o $@ mkstream.c

mktap: mktap.c
	$(CC) $(CFLAGS) -o $@ mktap.c

words2pt: words2pt.c
	$(CC) $(CFLAGS) -o $@ words2pt.c

pdp10-dec-none-ar:
	$(LN_S) $(TARGET_AR) $@

pdp10-dec-none-ranlib:
	$(LN_S) $(TARGET_RANLIB) $@

clean:
	$(RM) $(CTOOLS) $(ALIASES) *.o
	$(RM) -r __pycache__ legacy/__pycache__

install: all
	$(INSTALL) -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(DATADIR)/simh
	$(INSTALL) -m 755 $(CTOOLS) $(SCRIPTS) $(DESTDIR)$(BINDIR)/
	$(LN_S) $(TARGET_AR) $(DESTDIR)$(BINDIR)/pdp10-dec-none-ar
	$(LN_S) $(TARGET_RANLIB) $(DESTDIR)$(BINDIR)/pdp10-dec-none-ranlib
	$(INSTALL) -m 755 $(LEGACY) $(DESTDIR)$(BINDIR)/mkrim.py
	$(INSTALL) -m 644 $(SIMH_INIS) $(DESTDIR)$(DATADIR)/simh/
	@echo "Installed PDP-10 compatibility tools to $(DESTDIR)$(BINDIR)"

uninstall:
	@for f in $(CTOOLS) $(SCRIPTS) pdp10-dec-none-ar pdp10-dec-none-ranlib mkrim.py; do \
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
	@echo "  TARGET_AR=ar        symlink target for pdp10-dec-none-ar"
	@echo "  TARGET_RANLIB=ranlib symlink target for pdp10-dec-none-ranlib"
