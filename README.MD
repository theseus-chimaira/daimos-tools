# PDP-10 Compatibility Tools

This repository contains compatibility wrappers, standalone media/image
builders, and simulator helper tools for the DAIMOS/PDP-10 toolchain.

## Tools

- `p10run`: small PDP-6/KA10/KI10/KS10/KL10 SIMH bare-metal test runner.
- `mkdsk`: creates small PDP-6 DSK270 DBOOT V1 test bootsets.
- `mkdt`: writes PDP-6 18-bit SIMH DECtape images.
- `mkstream`: wraps assembled standalone words as a Stage1 stream.
- `mktap`: writes PDP-6 36-bit words as a SIMH magnetic-tape record.
- `words2pt`: encodes octal 36-bit words as PDP-6 paper-tape bytes.
- `darc`: builds indexed DARC1 target archives.
- `dlink`: links DOBJ1 objects and DARC1 archives to DXR V1.
- `pdp10-dec-none-darc`: target archive alias for `darc`.
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
