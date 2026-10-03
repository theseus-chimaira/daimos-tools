PDP10_PREFIX ?= /usr/local
BINDIR = ${PDP10_PREFIX}/bin
DATADIR = ${PDP10_PREFIX}/share/daimos-tools
INCLUDEDIR = ${PDP10_PREFIX}/include
CC = cc
CFLAGS = -O2

TOOLS = d6lz mkdsk mkd6fs d6fsck packfs d6bad logstore d6swap mkdt dta2dtr mktsfs tsfscheck mkstream mktap words2pt dlink darc p10run p10job pdp10-objdump p10fold p10super mkinitfs0 mkbootbanner sixmd-check
ALIASES = pdp10-dec-none-darc pdp10-dec-none-objdump mkinitfs
SIMH_INIS = simh/pdp6.ini simh/pdp10-ka.ini simh/pdp10-ki.ini simh/pdp10-kl.ini simh/pdp10-ks.ini
SIMH_NAMES = pdp6.ini pdp10-ka.ini pdp10-ki.ini pdp10-kl.ini pdp10-ks.ini
STALE_TOOLS = pdp10-dec-none-ar pdp10-dec-none-ranlib dxr2rim mkrim mkrim.py p10bare p10bare.py

DAIMOS_REPO ?= ../DAIMOS
DAIMOS_REPO_ABS := $(abspath ${DAIMOS_REPO})
NATIVE_BUILD_DIR ?= build-native-v1
NATIVE_BUILD_ABS := $(abspath ${NATIVE_BUILD_DIR})
PDP10_KCC ?= ${PDP10_PREFIX}/bin/kcc
PDP10_DAS ?= ${PDP10_PREFIX}/bin/das
PDP10_DLINK ?= ${PDP10_PREFIX}/bin/dlink
SIXMD_CHECK ?= ${PDP10_PREFIX}/bin/sixmd-check
NATIVE_KCCFLAGS ?= -Pgnu99 -O -x=pdp6 -m=gas
NATIVE_CPPFLAGS = \
	-I${DAIMOS_REPO_ABS}/system/kernel/boot \
	-I${DAIMOS_REPO_ABS}/system/kernel/core \
	-I${DAIMOS_REPO_ABS}/system/kernel/drivers \
	-I${DAIMOS_REPO_ABS}/system/kernel/fs \
	-I${DAIMOS_REPO_ABS}/system/kernel/mm \
	-I${DAIMOS_REPO_ABS}/system/kernel/modules \
	-I${DAIMOS_REPO_ABS}/system/kernel/proc \
	-I${DAIMOS_REPO_ABS}/system/kernel/storage \
	-I${DAIMOS_REPO_ABS}/userland/libc \
	-I${PDP10_PREFIX}/include
NATIVE_COMMON = \
	${NATIVE_BUILD_DIR}/crt0-v1.dobj \
	${NATIVE_BUILD_DIR}/syscall-v1.dobj \
	${NATIVE_BUILD_DIR}/syscall-helpers-v1.dobj \
	${NATIVE_BUILD_DIR}/u-v1.dobj \
	${NATIVE_BUILD_DIR}/dobj-native-v1.dobj
NATIVE_PROGRAMS = ${NATIVE_BUILD_DIR}/darc.dxr \
	${NATIVE_BUILD_DIR}/dlink.dxr ${NATIVE_BUILD_DIR}/objdump.dxr
NATIVE_MANUALS = DARC.SIXMD DLINK.SIXMD OBJDUMP.SIXMD

all: ${TOOLS}

.PHONY: native native-check

native: native-manual-check ${NATIVE_PROGRAMS}

.PHONY: native-manual-check
native-manual-check: ${NATIVE_MANUALS}
	${SIXMD_CHECK} DARC.SIXMD DARC
	${SIXMD_CHECK} DLINK.SIXMD DLINK
	${SIXMD_CHECK} OBJDUMP.SIXMD OBJDUMP

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

dta2dtr: dta2dtr.c
	${CC} ${CFLAGS} -std=c99 -Wall -Wextra -Werror -o $@ dta2dtr.c

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

p10job: p10job.c
	${CC} ${CFLAGS} -std=c99 -Wall -Wextra -Werror -o $@ p10job.c

pdp10-objdump: pdp10-objdump.c dobj.c dobj.h
	${CC} ${CFLAGS} -o $@ pdp10-objdump.c dobj.c

${NATIVE_BUILD_DIR}/%-v1.s: native/%.c native/dobj_native.h
	mkdir -p ${NATIVE_BUILD_DIR}
	${PDP10_KCC} ${NATIVE_KCCFLAGS} ${NATIVE_CPPFLAGS} -S $< -o $@

${NATIVE_BUILD_DIR}/dobj-native-v1.s: native/dobj_native.c native/dobj_native.h
	mkdir -p ${NATIVE_BUILD_DIR}
	${PDP10_KCC} ${NATIVE_KCCFLAGS} ${NATIVE_CPPFLAGS} -S $< -o $@

${NATIVE_BUILD_DIR}/u-v1.s: ${DAIMOS_REPO}/userland/libc/u.c
	mkdir -p ${NATIVE_BUILD_DIR}
	cd ${DAIMOS_REPO_ABS}/userland && ${PDP10_KCC} ${NATIVE_KCCFLAGS} \
		${NATIVE_CPPFLAGS} -S libc/u.c -o ${NATIVE_BUILD_ABS}/u-v1.s

${NATIVE_BUILD_DIR}/%-v1.dobj: ${NATIVE_BUILD_DIR}/%-v1.s
	${PDP10_DAS} -F -C -O $@ $<

${NATIVE_BUILD_DIR}/crt0-v1.dobj: ${DAIMOS_REPO}/userland/libc/crt0.s
	mkdir -p ${NATIVE_BUILD_DIR}
	${PDP10_DAS} -F -C -O $@ $<

${NATIVE_BUILD_DIR}/syscall-v1.dobj: ${DAIMOS_REPO}/userland/libc/syscall.s
	mkdir -p ${NATIVE_BUILD_DIR}
	${PDP10_DAS} -F -C -O $@ $<

${NATIVE_BUILD_DIR}/syscall-helpers-v1.dobj: ${DAIMOS_REPO}/userland/libc/syscall_helpers.s
	mkdir -p ${NATIVE_BUILD_DIR}
	${PDP10_DAS} -F -C -O $@ $<

${NATIVE_BUILD_DIR}/darc.dxr: ${NATIVE_COMMON} ${NATIVE_BUILD_DIR}/darc_native-v1.dobj
	${PDP10_DLINK} --daimos-uuo-relax -b 020 -o $@ -M ${NATIVE_BUILD_DIR}/darc.map \
		${NATIVE_COMMON} ${NATIVE_BUILD_DIR}/darc_native-v1.dobj

${NATIVE_BUILD_DIR}/dlink.dxr: ${NATIVE_COMMON} ${NATIVE_BUILD_DIR}/dlink_native-v1.dobj
	${PDP10_DLINK} --daimos-uuo-relax -b 020 -o $@ -M ${NATIVE_BUILD_DIR}/dlink.map \
		${NATIVE_COMMON} ${NATIVE_BUILD_DIR}/dlink_native-v1.dobj

${NATIVE_BUILD_DIR}/objdump.dxr: ${NATIVE_COMMON} ${NATIVE_BUILD_DIR}/objdump_native-v1.dobj
	${PDP10_DLINK} --daimos-uuo-relax -b 020 -o $@ -M ${NATIVE_BUILD_DIR}/objdump.map \
		${NATIVE_COMMON} ${NATIVE_BUILD_DIR}/objdump_native-v1.dobj

p10fold: p10fold.c dobj.c dobj.h
	${CC} ${CFLAGS} -std=c99 -Wall -Wextra -Werror -o $@ p10fold.c dobj.c

p10super: p10super.c dobj.c dobj.h
	${CC} ${CFLAGS} -std=c99 -Wall -Wextra -Werror -o $@ p10super.c dobj.c

mkinitfs0: mkinitfs0.c
	${CC} ${CFLAGS} -o $@ mkinitfs0.c

mkbootbanner: mkbootbanner.c
	${CC} ${CFLAGS} -o $@ mkbootbanner.c

sixmd-check: sixmd-check.c
	${CC} ${CFLAGS} -std=c99 -Wall -Wextra -Werror -o $@ sixmd-check.c

clean:
	rm -f ${TOOLS} *.o
	rm -rf ${NATIVE_BUILD_DIR}

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
	@echo "DAIMOS TOOLS"
	@echo "  MAKE"
	@echo "  MAKE INSTALL PDP10_PREFIX=/PATH"
	@echo "  MAKE UNINSTALL PDP10_PREFIX=/PATH"
	@echo "  MAKE CLEAN"
