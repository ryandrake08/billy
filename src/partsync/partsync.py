#!/usr/bin/env python3
"""partsync — Symbol Fields Table sync for board/billy/billy.kicad_sch.

`report` prints the vendor-relevant fields for every symbol that carries an "LCSC Part" value;
everything else is skipped, since that field is what vendor sourcing lookups key off.
`set-field` patches a single field on one symbol, in place — or creates it (`--create`) if the
symbol has no property for that field yet.
"""
from __future__ import annotations

import argparse
import difflib
import sys
from pathlib import Path

from patch import SchematicLockedError, UnknownFieldError, patch_field, write_field
from schematic import parse_schematic

REPORT_FIELDS = [
    "Value", "LCSC Part", "MF", "MP", "Description",
    "DigiKey Part", "DigiKey MF", "DigiKey MP",
    "Mouser Part", "Mouser MF", "Mouser MP",
]

DEFAULT_SCHEMATIC = Path(__file__).resolve().parents[2] / "board" / "billy" / "billy.kicad_sch"


def cmd_report(args: argparse.Namespace) -> int:
    symbols = parse_schematic(args.schematic)
    tracked = [s for s in symbols if s.fields.get("LCSC Part")]

    if args.ref:
        tracked = [s for s in tracked if s.reference == args.ref]
        if not tracked:
            print(f"no symbol with Reference={args.ref!r} and a set LCSC Part", file=sys.stderr)
            return 1

    for sym in tracked:
        print(f"{sym.reference} ({sym.lib_id})")
        for name in REPORT_FIELDS:
            value = sym.fields.get(name, "")
            print(f"  {name:<14} {value}")
        print()

    if not args.ref:
        skipped = len(symbols) - len(tracked)
        print(f"{len(tracked)} symbol(s) with LCSC Part set, {skipped} skipped (no LCSC Part)", file=sys.stderr)
    return 0


def cmd_set_field(args: argparse.Namespace) -> int:
    try:
        old_text, new_text = patch_field(args.schematic, args.ref, args.field, args.value, create=args.create)
    except (KeyError, UnknownFieldError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    if old_text == new_text:
        print("no change")
        return 0

    diff = difflib.unified_diff(
        old_text.splitlines(keepends=True), new_text.splitlines(keepends=True),
        fromfile=str(args.schematic), tofile=str(args.schematic),
    )
    sys.stdout.writelines(diff)

    if not args.write:
        print("(dry run — pass --write to apply)", file=sys.stderr)
        return 0

    try:
        write_field(args.schematic, args.ref, args.field, args.value, create=args.create, force=args.force)
    except SchematicLockedError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    print(f"wrote {args.schematic}", file=sys.stderr)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Sync billy.kicad_sch's vendor fields against JLCPCB/DigiKey/Mouser.")
    parser.add_argument("--schematic", type=Path, default=DEFAULT_SCHEMATIC, help="path to the .kicad_sch file")
    sub = parser.add_subparsers(dest="command", required=True)

    report = sub.add_parser("report", help="print current vendor fields (read-only)")
    report.add_argument("--ref", help="limit to a single symbol reference, e.g. C11")
    report.set_defaults(func=cmd_report)

    set_field = sub.add_parser("set-field", help="patch (or create) one field on one symbol")
    set_field.add_argument("--ref", required=True, help="symbol reference, e.g. C11")
    set_field.add_argument("--field", required=True, help='field name, e.g. "MF"')
    set_field.add_argument("--value", required=True, help="new value")
    set_field.add_argument("--create", action="store_true", help="insert the property if the symbol doesn't have it yet")
    set_field.add_argument("--write", action="store_true", help="apply the change (default is dry-run diff only)")
    set_field.add_argument("--force", action="store_true", help="write even if KiCad's lock file is present")
    set_field.set_defaults(func=cmd_set_field)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
