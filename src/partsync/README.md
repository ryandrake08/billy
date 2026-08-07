# partsync — Symbol Fields Table sync

Deterministic (no LLM) sync of `board/billy/billy.kicad_sch`'s vendor fields (MF, MP,
Description, and per-vendor Part/MF/MP) against JLCPCB, DigiKey, and Mouser, keyed off each
symbol's "LCSC Part" field.

## report (read-only)

No external dependencies — plain stdlib. `sexpr.py` is a small S-expression parser (with
byte-offset tracking on every atom, to support patching a value in place later without
reformatting the rest of the file) that reads `.kicad_sch`/`.kicad_pcb`-style files;
`schematic.py` walks it into per-symbol field dictionaries, skipping the `lib_symbols` cache
block.

```bash
python3 partsync.py report              # every symbol with an "LCSC Part" set
python3 partsync.py report --ref C11     # just one symbol
```

Symbols with no "LCSC Part" value are skipped — that field is what vendor lookups key off, so
a symbol without it has nothing to sync.

## set-field (patch or create one field)

`patch.py` splices a single property's value in place by byte offset, so a change touches only
that one value and nothing else in the file reflows. By default it only patches fields that
already exist on a symbol; `--create` inserts a brand-new property instead, rendered to match
KiCad's own hidden-field format exactly (same `(at ...)`, hidden, default font as any other
non-visible field). Refuses to write if KiCad's own lock file (`~<name>.kicad_sch.lck`) is
present, unless `--force`.

```bash
python3 partsync.py set-field --ref C11 --field MF --value "Samsung"                       # dry-run diff
python3 partsync.py set-field --ref C11 --field MF --value "Samsung" --write               # apply it
python3 partsync.py set-field --ref C11 --field "Mouser Part" --value "..." --create --write   # field doesn't exist yet
```

## Tests

No pytest dependency, matching `client/`/`shim/`'s convention — run directly:

```bash
python3 test_sexpr.py
python3 test_schematic.py
python3 test_patch.py
```
