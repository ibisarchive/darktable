# Ibis Archive

Ibis Archive is darktable with two additions for bird photographers:

1. **Identify birds** — an on-device classifier (ONNX, through darktable's
   own AI backend) that tags each frame `Birds|Species|<name>`.
2. **eBird checklist** — an export storage that turns a selection of
   frames into eBird's Record Format CSV (species, place, position, date,
   start, duration), plus the JPEGs sorted per species for upload.

Everything else — maps, GPX correlation, geotagging, tags, albums, XMP,
editing, export presets — is darktable, unchanged.

## Rule of the fork

The core stays upstream's. Fork code lives in:

- `src/libs/ibis_*.c`      lighttable modules
- `data/lua/ibis/`         bundled Lua (`data/luarc` requires it, guarded)
- `data/ibis/`             models manifest, taxonomy, branding assets
- `packaging/`             installer name, icon, publisher

`ibis/main` is rebased on `upstream/master` regularly. Anything that would be
useful to every darktable user is sent upstream instead of kept here.

## Build (Windows)

MSYS2 UCRT64, per `packaging/windows/README.md`:

    ./build.sh --prefix /opt/darktable --build-type Release --build-generator Ninja --install
    cmake --build build --target package

## Licence

GPL-3.0-or-later, as darktable. Copyright (C) darktable developers;
Ibis Archive additions Copyright (C) 2026 Ibis Archive contributors.
