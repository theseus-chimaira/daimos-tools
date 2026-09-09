# PDP-10 Compatibility Tools

This repository contains compatibility wrappers, standalone media/image
builders, and simulator helper tools for the DAIMOS/PDP-10 toolchain.

## Tools

- `p10run`: small PDP-6/KA10/KI10/KS10/KL10 SIMH bare-metal test runner.
- `mkdsk`: creates small PDP-6 DSK270 DBOOT V1 test bootsets.
- `mkd6fs`: formats D6FS V2 inside a DBOOT diskset using its root-layout descriptor.
- `mkdt`: writes PDP-6 18-bit SIMH DECtape images.
- `mkstream`: wraps assembled standalone words as a Stage1 stream.
- `mktap`: writes PDP-6 36-bit words as an odd-parity 7-track SIMH magnetic-tape record.
- `words2pt`: encodes octal 36-bit words as PDP-6 paper-tape bytes.
- `darc`: builds indexed DARC1 target archives.
- `dlink`: links DOBJ1 objects and DARC1 archives to DXR V1.
- `pdp10-dec-none-darc`: target archive alias for `darc`.
- `mkinitfs0`: builds hierarchical DAIMOS INITFS0 images as PDP-10 assembly or octal words.
- `mkinitfs`: compatibility alias for `mkinitfs0`.
- `simh/*.ini`: SIMH configuration files used by `p10run`.

The assembler itself lives in the DAS repository. Install DAS and these tools
under the same `PDP10_PREFIX` when using the full testkit/toolchain flow.

DXR conversion, including standard RIM10B and paper-tape output, is owned by
DAS `dxrconvert`.  The older `dxr2rim`, `mkrim`, and Python `mkrim.py`
frontends were removed after `p10run` moved to the DOBJ/DARC/DLINK -> DXR ->
`dxrconvert` pipeline.

## Build And Install

Build the host C tools and target aliases:

```sh
make
```

Install under a prefix:

```sh
make install PDP10_PREFIX=/path/to/prefix
```

If both `PREFIX` and `PDP10_PREFIX` are set, `PDP10_PREFIX` takes precedence.
The obsolete target `ar` and `ranlib` aliases are deliberately removed.

Remove installed files:

```sh
make uninstall PDP10_PREFIX=/path/to/prefix
```

## License

These tools are distributed under the MIT License. See `LICENCE`.

## DOBJ1 and DARC1

`dlink` is the minimal DAIMOS static linker.  It consumes DOBJ1 relocatable
objects and DARC1 static archives and emits the existing DXR V1 executable
format.  DOBJ1 deliberately carries only semantic TEXT/DATA/BSS sections,
global definitions/imports, optional entry selection, and RH18/LH18 relocations.
Local assembler labels are resolved before object emission and are not part of
the serialized symbol namespace.

`darc` builds a DARC1 archive from DOBJ1 members.  Its index maps exported
symbols directly to member offsets so `dlink` can extract only members needed
by currently unresolved globals.  This is the required model for GCC libgcc:
private labels may be reused in different archive members without collision.

The first format revision intentionally excludes debug records, weak symbols,
dynamic linking, named sections, and DEC REL compatibility.  TEXT/DATA/BSS
identity is preserved by the linker so later DAIMOS shared-text and fixed-slot
overlay outputs do not require an object-format redesign.

For DAIMOS bootstrap modules, `dlink -R package.dobj -N package_symbol` can
also emit a compact relocatable resident-image package while doing the normal
link once at base zero.  Repeated `-X symbol` options export offsets from that
package.  The package contains initialized words, BSS size, and a two-bit-per-
word LH18/RH18 relocation map; absolute definitions supplied with `-A map` are
resolved normally and are not marked for runtime relocation.  This does not
require a second compile or sizing link pass.


## DXR purity contract

`dlink` records executable text purity in the high flag bits of the DXR1 BSS
halfword.  `--pure` is an explicit program contract: executable text will not
be modified at runtime.  `--impure` forces the conservative state.  Without
either option the state is UNKNOWN unless the linker can prove a direct,
unindexed, non-indirect store into final TEXT, in which case it records
IMPURE automatically.  UNKNOWN is not a proof of purity and must be treated
like IMPURE by swapping code.  A proven text write contradicts `--pure` and
causes the link to fail.

## DAIMOS direct-UUO link relaxation

`dlink --daimos-uuo-relax` may replace an exact direct
`PUSHJ 17,dsys_*` relocation with the corresponding native DAIMOS monitor
UUO.  The mode recognizes only libc veneers whose complete ABI action is one
UUO followed by `POPJ 17,`; helpers which adjust arguments are not eligible.

If every reference to an eligible veneer is a direct relaxable call, the
symbol may remain undefined and no veneer object is required.  Any address
reference, indirect/indexed call, or nonzero relocation addend forces normal
symbol resolution so function pointers retain a callable definition.
