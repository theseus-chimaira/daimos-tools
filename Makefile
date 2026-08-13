PREFIX ?= /usr/local
ifdef PDP10_PREFIX
override PREFIX := $(PDP10_PREFIX)
endif
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share/pdp10-tools

INSTALL ?= install
LN_S ?= ln -sf
RM ?= rm -f
CC ?= cc
CFLAGS ?= -Wall -Wextra -O2 -std=c89

CTOOLS := dxr2rim mkdsk mkdt mkrim mkstream mktap words2pt
SCRIPTS := p10bare
TARGET_AR ?= ar
TARGET_RANLIB ?= ranlib
LEGACY := legacy/mkrim.py
SIMH_INIS := simh/pdp6.ini simh/pdp10-ka.ini simh/pdp10-ki.ini \
	simh/pdp10-kl.ini simh/pdp10-ks.ini

.PHONY: all clean install uninstall help

all: $(CTOOLS)

dxr2rim: dxr2rim.c
	$(CC) $(CFLAGS) -o $@ $<

mkdsk: mkdsk.c
	$(CC) $(CFLAGS) -o $@ $<

mkdt: mkdt.c
	$(CC) $(CFLAGS) -o $@ $<

mkrim: mkrim.c
	$(CC) $(CFLAGS) -o $@ $<

mkstream: mkstream.c
	$(CC) $(CFLAGS) -o $@ $<

mktap: mktap.c
	$(CC) $(CFLAGS) -o $@ $<

words2pt: words2pt.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	$(RM) $(CTOOLS) *.o
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
	$(RM) $(addprefix $(DESTDIR)$(BINDIR)/,$(CTOOLS) $(SCRIPTS) pdp10-dec-none-ar pdp10-dec-none-ranlib)
	$(RM) $(DESTDIR)$(BINDIR)/mkrim.py
	$(RM) $(addprefix $(DESTDIR)$(DATADIR)/simh/,$(notdir $(SIMH_INIS)))
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
	@echo "  PREFIX=/usr/local   installation prefix"
	@echo "  PDP10_PREFIX=...    PDP-10 toolchain prefix; overrides PREFIX"
	@echo "  TARGET_AR=ar        symlink target for pdp10-dec-none-ar"
	@echo "  TARGET_RANLIB=ranlib symlink target for pdp10-dec-none-ranlib"
