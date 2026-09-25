PDP10_PREFIX ?= /usr/local
BINDIR = ${PDP10_PREFIX}/bin
DATADIR = ${PDP10_PREFIX}/share/pdp10-tools
INCLUDEDIR = ${PDP10_PREFIX}/include
CC = cc
CFLAGS = -O2

TOOLS = d6lz mkdsk mkd6fs d6fsck packfs d6bad logstore d6swap mkdt mktsfs tsfscheck mkstream mktap words2pt dlink darc p10run pdp10-objdump mkinitfs0 mkbootbanner
ALIASES = pdp10-dec-none-darc pdp10-dec-none-objdump mkinitfs
SIMH_INIS = simh/pdp6.ini simh/pdp10-ka.ini simh/pdp10-ki.ini simh/pdp10-kl.ini simh/pdp10-ks.ini
SIMH_NAMES = pdp6.ini pdp10-ka.ini pdp10-ki.ini pdp10-kl.ini pdp10-ks.ini
STALE_TOOLS = pdp10-dec-none-ar pdp10-dec-none-ranlib dxr2rim mkrim mkrim.py p10bare p10bare.py

all: ${TOOLS}

d6lz: d6lz.c
	${CC} ${CFLAGS} -std=c99 -Wall -Wextra -Werror -o $@ d6lz.c

mkdsk: mkdsk.c
	${CC} ${CFLAGS} -o $@ mkdsk.c

mkd6fs: mkd6fs.c d6maint.c d6maint.h
	${CC} ${CFLAGS} -o $@ mkd6fs.c d6maint.c

d6fsck: d6fsck.c
	${CC} ${CFLAGS} -o $@ d6fsck.c

packfs: packfs.c d6maint.c d6maint.h
	${CC} ${CFLAGS} -o $@ packfs.c d6maint.c

d6bad: d6bad.c d6maint.c d6maint.h
	${CC} ${CFLAGS} -o $@ d6bad.c d6maint.c

logstore: logstore.c d6maint.c d6maint.h
	${CC} ${CFLAGS} -o $@ logstore.c d6maint.c

d6swap: d6swap.c d6maint.c d6maint.h
	${CC} ${CFLAGS} -o $@ d6swap.c d6maint.c

mkdt: mkdt.c
	${CC} ${CFLAGS} -o $@ mkdt.c

mktsfs: mktsfs.c tsfs-format.c tsfs-format.h d6lz-codec.c d6lz-codec.h
	${CC} ${CFLAGS} -std=c99 -o $@ mktsfs.c tsfs-format.c d6lz-codec.c

tsfscheck: tsfscheck.c tsfs-format.c tsfs-format.h d6lz-codec.c d6lz-codec.h
	${CC} ${CFLAGS} -std=c99 -o $@ tsfscheck.c tsfs-format.c d6lz-codec.c

mkstream: mkstream.c
	${CC} ${CFLAGS} -o $@ mkstream.c

mktap: mktap.c
	${CC} ${CFLAGS} -o $@ mktap.c

words2pt: words2pt.c
	${CC} ${CFLAGS} -o $@ words2pt.c

dlink: dlink.c dobj.c dobj.h
	${CC} ${CFLAGS} -o $@ dlink.c dobj.c

darc: darc.c dobj.c dobj.h
	${CC} ${CFLAGS} -o $@ darc.c dobj.c

p10run: p10run.c
	${CC} ${CFLAGS} -o $@ p10run.c

pdp10-objdump: pdp10-objdump.c dobj.c dobj.h
	${CC} ${CFLAGS} -o $@ pdp10-objdump.c dobj.c

mkinitfs0: mkinitfs0.c
	${CC} ${CFLAGS} -o $@ mkinitfs0.c

mkbootbanner: mkbootbanner.c
	${CC} ${CFLAGS} -o $@ mkbootbanner.c

clean:
	rm -f ${TOOLS} *.o

install: all
	mkdir -p "${DESTDIR}${BINDIR}" "${DESTDIR}${DATADIR}/simh" "${DESTDIR}${INCLUDEDIR}"
	for f in ${STALE_TOOLS}; do rm -f "${DESTDIR}${BINDIR}/$$f"; done
	for f in ${TOOLS}; do cp "$$f" "${DESTDIR}${BINDIR}/$$f"; chmod 755 "${DESTDIR}${BINDIR}/$$f"; done
	ln -sf darc "${DESTDIR}${BINDIR}/pdp10-dec-none-darc"
	ln -sf pdp10-objdump "${DESTDIR}${BINDIR}/pdp10-dec-none-objdump"
	ln -sf mkinitfs0 "${DESTDIR}${BINDIR}/mkinitfs"
	cp ${SIMH_INIS} "${DESTDIR}${DATADIR}/simh/"
	chmod 644 "${DESTDIR}${DATADIR}"/simh/*
	cp pdp10-sixbit.h tsfs-format.h "${DESTDIR}${INCLUDEDIR}/"
	chmod 644 "${DESTDIR}${INCLUDEDIR}/pdp10-sixbit.h" "${DESTDIR}${INCLUDEDIR}/tsfs-format.h"

uninstall:
	for f in ${TOOLS} ${ALIASES} ${STALE_TOOLS}; do rm -f "${DESTDIR}${BINDIR}/$$f"; done
	for f in ${SIMH_NAMES}; do rm -f "${DESTDIR}${DATADIR}/simh/$$f"; done
	rm -f "${DESTDIR}${INCLUDEDIR}/pdp10-sixbit.h" "${DESTDIR}${INCLUDEDIR}/tsfs-format.h"

help:
	@echo "PDP10 TOOLS"
	@echo "  MAKE"
	@echo "  MAKE INSTALL PDP10_PREFIX=/PATH"
	@echo "  MAKE UNINSTALL PDP10_PREFIX=/PATH"
	@echo "  MAKE CLEAN"
