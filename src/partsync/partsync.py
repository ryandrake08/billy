#!/usr/bin/env python3
"""partsync — Symbol Fields Table sync for board/billy/billy.kicad_sch.

Read-only. `report` prints the vendor-relevant fields for every
symbol that carries an "LCSC Part" value; everything else is skipped, since
that field is what vendor sourcing lookups key off.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

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


def main() -> int:
    parser = argparse.ArgumentParser(description="Sync billy.kicad_sch's vendor fields against JLCPCB/DigiKey/Mouser.")
    parser.add_argument("--schematic", type=Path, default=DEFAULT_SCHEMATIC, help="path to the .kicad_sch file")
    sub = parser.add_subparsers(dest="command", required=True)

    report = sub.add_parser("report", help="print current vendor fields (read-only)")
    report.add_argument("--ref", help="limit to a single symbol reference, e.g. C11")
    report.set_defaults(func=cmd_report)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
