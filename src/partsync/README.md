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

## Tests

No pytest dependency, matching `client/`/`shim/`'s convention — run directly:

```bash
python3 test_sexpr.py
python3 test_schematic.py
```
