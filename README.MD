# PDP10 TOOLS

PDP10 TOOLS IS THE HOST SIDE SUPPORT TOOL COLLECTION FOR THE PDP-6 AND PDP-10 TOOLCHAIN AND FOR DAIMOS IMAGE AND STORAGE FORMATS.

## p10super

`p10super` is a host-side PDP-10 scheduling-similarity analyzer used to
discover source-layout changes which may expose additional exact `p10fold`
opportunities. It models a conservative subset of PDP-10 register, memory,
half-word, compare and test semantics, and searches legal instruction
schedules within bounded straight-line regions.

For proven one-word peepholes, `p10super` emits `REWRITE` records with a
symbol-relative source location, the original and replacement assembler form,
the exact one-word saving, zero execution-cost delta, and the legality proof.
For scheduling-equivalent regions it emits `REORDER` records naming both fold
partners and the concrete old/new instruction-offset order on each side.
Reordering itself claims no size saving; `FOLD-POTENTIAL` is only the region
size which must still be reassembled and verified by `p10fold`.

The tool is advisory: `SCHEDULE` results require no AC renaming. `RENAME`
results require a consistent AC renaming and are emitted only when each
renamed AC is conservatively proven local to the compared region: its incoming
value is killed before use and its outgoing value is killed before a later
read. Unknown instructions and control-flow boundaries stop that proof.
`CANON-ZERO` reports equal-cost normalization between an unlabelled
`MOVEI AC,0` and `SETZ AC`; labelled/alternate-entry instructions are not
rewritten. The peephole pass also reports strictly smaller PDP-10 forms such as
`CAIN AC,0 ; JRST` -> `JUMPE` and increment/decrement plus jump -> AOJ/SOJ.
CFG comparison also recognizes equivalent subgraphs whose terminal
JUMP/AOJ/SOJ conditions are complementary and whose successors are therefore
exchanged. Neither advisory class is folded directly; changed source must still
assemble and pass the exact relocation-aware `p10fold` checks.

`p10fold` searches exact relocation-aware duplicate text. Its zero-cost
accounting distinguishes removable unnamed donor blocks from address-stable
anchors and also considers aligned removable subruns inside longer duplicate
runs, so a non-removable maximal match does not hide a legal shorter fold.
With `-P fold.plan`, it writes the selected non-overlapping zero-cost folds as
a `P10FOLD1` plan. `dlink --fold-plan fold.plan` consumes that plan while
laying out TEXT, omits donor words, retargets references to the retained anchor,
and shifts later symbols and relocations through the compacted TEXT map.  DLINK
REVALIDATES EACH PLAN RANGE WITH THE SAME RELOCATION-AWARE EQUALITY CONTRACT
BEFORE REMOVING TEXT, SO A STALE PLAN CANNOT SILENTLY DELETE CHANGED CODE.

THE DAS ASSEMBLER AND THE TEST SUITES ARE MAINTAINED IN SEPARATE PROJECTS.

## BUILD

BUILD ALL HOST TOOLS WITH:

    MAKE

THE MAKEFILE USES PORTABLE MAKE SYNTAX AND BUILDS C99 HOST PROGRAMS.

INSTALL UNDER THE PDP-10 TOOLCHAIN PREFIX WITH:

    MAKE INSTALL PDP10_PREFIX=/PATH/TO/PREFIX

REMOVE THE INSTALLED FILES WITH:

    MAKE UNINSTALL PDP10_PREFIX=/PATH/TO/PREFIX

HOST UNIX COMMANDS AND FILE NAMES ARE PRINTED IN UPPER CASE BECAUSE THIS DOCUMENT IS SIXBIT CLEAN.  ENTER THEIR NORMAL HOST SPELLING WHEN REQUIRED.

## TOOLS

- P10RUN - SIMH BARE METAL TEST RUNNER FOR PDP-6 AND PDP-10 SYSTEMS.
- DLINK - STATIC LINKER FOR DOBJ1 OBJECTS AND DARC1 ARCHIVES.
- DARC - DARC1 STATIC ARCHIVE BUILDER.
- PDP10-OBJDUMP - DOBJ1 OBJECT INSPECTION TOOL.
- P10FOLD - RELOCATION-AWARE DOBJ1 DUPLICATE TEXT ANALYZER.
- MKDSK - DSK270 DBOOT DISKSET BUILDER.
- MKD6FS - D6FS FILESYSTEM BUILDER.
- D6FSCK - D6FS CHECK AND REPAIR TOOL.
- PACKFS - D6FS PACKING AND COMPACTION TOOL.
- D6BAD - D6FS STORAGE-SET PHYSICAL SOURCE->SPARE BADMAP TOOL.
- LOGSTORE - D6FS LOGSTORE MAINTENANCE TOOL.
- D6SWAP - D6FS SWAP AREA INSPECTION AND MAINTENANCE TOOL.
- MKDT - SIMH DECTAPE IMAGE BUILDER.
- MKTSFS - MINIMAL TSFS DECTAPE SET BUILDER.
- TSFSCHECK - TSFS MEMBER/DESCRIPTOR AND TABLE-DIRECTORY VERIFIER.
- MKSTREAM - STAGE1 STREAM BUILDER.
- MKTAP - SIMH 7 TRACK MAGNETIC TAPE RECORD BUILDER.
- WORDS2PT - PDP-6 PAPER TAPE ENCODER.
- MKINITFS0 - DAIMOS INITFS0 IMAGE BUILDER.
- MKBOOTBANNER - DAIMOS BOOT BANNER GENERATOR.

INSTALLED COMPATIBILITY NAMES ARE PDP10-DEC-NONE-DARC, PDP10-DEC-NONE-OBJDUMP, AND MKINITFS.

## FORMATS

DOBJ1 IS THE RELOCATABLE OBJECT FORMAT USED BY DAS AND DLINK.

DARC1 IS THE STATIC ARCHIVE FORMAT USED BY DLINK.

DXR V1 IS THE EXECUTABLE OUTPUT FORMAT USED BY THE CURRENT TOOLCHAIN.

D6FS, TSFS, INITFS0, DSK270 DBOOT, SIMH DECTAPE, SIMH MAGNETIC TAPE, AND PDP-6 PAPER TAPE SUPPORT ARE PROVIDED BY THE CORRESPONDING HOST TOOLS.

## TESTS

THIS REPOSITORY CONTAINS NO TESTS.

GENERIC PDP10 TOOLS REGRESSIONS LIVE IN PDP10-TESTKIT.

DAIMOS SPECIFIC PDP10 TOOLS REGRESSIONS LIVE IN DAIMOS-TESTKIT.

## DOCUMENTATION

STATUS.MD IS THE SOLE CURRENT STATE DOCUMENT.

TODO.MD CONTAINS ONLY OPEN PDP10 TOOLS WORK.

## LICENSE

THE PROJECT IS DISTRIBUTED UNDER THE MIT LICENSE.  SEE LICENCE.

DXR EXECUTABLES USE PURE AS A ONE-BIT TEXT POLICY.  DLINK --RT-REQUIRED SETS THE 0400000 EXECUTABLE ADMISSION FLAG; THE FORMER IMPURE FLAG/--impure OPTION IS REMOVED.
