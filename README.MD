# PDP-10 Compatibility Tools

This repository contains compatibility wrappers, standalone media/image
builders, and simulator helper tools for the DAIMOS/PDP-10 toolchain.

## Tools

- `p10bare`: small PDP-6/KA10/KI10/KS10/KL10 SIMH bare-metal test runner.
- `dxr2rim`: converts DAIMOS DXR V1 to PDP-6 RIM tape.
- `mkdsk`: creates small PDP-6 DSK270 DBOOT V1 test bootsets.
- `mkdt`: writes PDP-6 18-bit SIMH DECtape images.
- `mkrim`: converts SIMH-style deposit text to PDP-6 executable RIM tape.
- `mkstream`: wraps assembled standalone words as a Stage1 stream.
- `mktap`: writes PDP-6 36-bit words as a SIMH magnetic-tape record.
- `words2pt`: encodes octal 36-bit words as PDP-6 paper-tape bytes.
- `pdp10-dec-none-ar`: target archive alias, installed as a symlink to `ar`.
- `pdp10-dec-none-ranlib`: target ranlib alias, installed as a symlink to `ranlib`.
- `simh/*.ini`: SIMH configuration files used by `p10bare`.

The assembler itself lives in the DAS repository. Install DAS and these tools
under the same `PDP10_PREFIX` when using the full testkit/toolchain flow.

## Deprecated Python Tools

The Python `p10bare` and `mkrim` are deprecated and will be replaced by C tools soon.

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
Override `TARGET_AR` or `TARGET_RANLIB` to change the symlink targets.

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
global definitions/imports, optional entry selection, and RH18 relocations.
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
