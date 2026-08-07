"""In-place field patching for .kicad_sch files — the write side of schematic.py's read path.

Patches an existing property's value by splicing the exact quoted-string span the parser found
for it, leaving every other byte of the file untouched. With `create=True`, a field that has no
existing property on a symbol gets a brand-new one inserted after that symbol's last property,
rendered to match KiCad's own hidden-field convention (placed at the symbol's own position,
hidden, default 1.27mm font) exactly — same as what "LCSC Part" or "Description" already look
like on any placed part.
"""
from __future__ import annotations

import os
from pathlib import Path

from schematic import Symbol, parse_schematic_text


class UnknownFieldError(KeyError):
    """Raised when a symbol has no existing property for the requested field name and
    creation wasn't requested."""


class SchematicLockedError(RuntimeError):
    """Raised when KiCad appears to have the schematic open (its lock file exists)."""


def escape_value(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def lock_path(schematic_path: Path) -> Path:
    return schematic_path.with_name(f"~{schematic_path.name}.lck")


def apply_field_updates(
    text: str, symbols: list[Symbol], updates: dict[str, dict[str, str]], *, create: bool = False
) -> str:
    """updates: {symbol_uuid: {field_name: new_value}}. Returns the patched file text."""
    by_uuid = {s.uuid: s for s in symbols}
    ops: list[tuple[int, int, str]] = []  # (start, end, replacement) — patches are real spans,
                                           # insertions are zero-width (start == end)
    for uuid, fields in updates.items():
        symbol = by_uuid.get(uuid)
        if symbol is None:
            raise KeyError(f"no symbol with uuid {uuid!r}")
        for name, value in fields.items():
            atom = symbol.field_atoms.get(name)
            if atom is not None:
                ops.append((atom.start, atom.end, escape_value(value)))
            elif create:
                if symbol.last_property_end < 0:
                    raise UnknownFieldError(f"symbol {uuid} ({symbol.reference}) has no properties to anchor a new one on")
                block = _render_new_property(name, value, symbol.at, symbol.property_indent)
                anchor = symbol.last_property_end
                ops.append((anchor, anchor, "\n" + block))
            else:
                raise UnknownFieldError(f"symbol {uuid} ({symbol.reference}) has no existing {name!r} property")

    ops.sort(key=lambda o: (o[0], o[1]))
    for (_, end, _), (next_start, _, _) in zip(ops, ops[1:]):
        if next_start < end:
            raise ValueError("overlapping patch/insert spans")

    out = []
    cursor = 0
    for start, end, replacement in ops:
        out.append(text[cursor:start])
        out.append(replacement)
        cursor = end
    out.append(text[cursor:])
    return "".join(out)


def _render_new_property(name: str, value: str, at: str, indent: str) -> str:
    i1 = indent + "\t"
    i2 = i1 + "\t"
    i3 = i2 + "\t"
    return (
        f'{indent}(property "{name}" {escape_value(value)}\n'
        f'{i1}(at {at})\n'
        f'{i1}(hide yes)\n'
        f'{i1}(show_name no)\n'
        f'{i1}(do_not_autoplace no)\n'
        f'{i1}(effects\n'
        f'{i2}(font\n'
        f'{i3}(size 1.27 1.27)\n'
        f'{i2})\n'
        f'{i1})\n'
        f'{indent})'
    )


def patch_field(schematic_path: Path, ref: str, field: str, value: str, *, create: bool = False) -> tuple[str, str]:
    """Pure read + compute, no write. Returns (old_text, new_text)."""
    text = schematic_path.read_text()
    symbols = parse_schematic_text(text)
    symbol = _find_by_reference(symbols, ref)
    new_text = apply_field_updates(text, symbols, {symbol.uuid: {field: value}}, create=create)
    return text, new_text


def write_field(
    schematic_path: Path, ref: str, field: str, value: str, *, create: bool = False, force: bool = False
) -> tuple[str, str]:
    """Patch (or create) one field and write the result to disk if it changed. Returns (old_text, new_text)."""
    lp = lock_path(schematic_path)
    if lp.exists() and not force:
        raise SchematicLockedError(f"{lp} exists — KiCad likely has this file open; pass force=True to override")
    old_text, new_text = patch_field(schematic_path, ref, field, value, create=create)
    if old_text != new_text:
        _write_atomic(schematic_path, new_text)
    return old_text, new_text


def _find_by_reference(symbols: list[Symbol], ref: str) -> Symbol:
    matches = [s for s in symbols if s.reference == ref]
    if not matches:
        raise KeyError(f"no symbol with Reference={ref!r}")
    if len(matches) > 1:
        raise KeyError(f"multiple symbols with Reference={ref!r}")
    return matches[0]


def _write_atomic(path: Path, text: str) -> None:
    tmp = path.with_name(path.name + ".partsync-tmp")
    tmp.write_text(text)
    os.replace(tmp, path)
