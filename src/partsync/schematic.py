"""Extract per-symbol field data from a KiCad schematic (.kicad_sch).

Only top-level `symbol` instances are read — the `lib_symbols` cache block
(KiCad's local copy of each library part's field defaults) also contains
nodes tagged `symbol`, but they sit one level deeper, under `lib_symbols`,
and never surface as direct children of the root node.
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from sexpr import Atom, Node, parse


@dataclass
class Symbol:
    uuid: str
    lib_id: str
    at: str                       # symbol's own "(at X Y ROT)", as "X Y ROT" — where a new
                                   # hidden field gets placed, matching KiCad's own convention
    fields: dict[str, str]        # field name -> current value
    field_atoms: dict[str, Atom]  # field name -> value Atom (byte offsets for patching)
    last_property_end: int        # offset just past the last existing property node — the
                                   # anchor point for inserting a new one; -1 if there are none
    property_indent: str          # this symbol's property lines' leading whitespace, so a new
                                   # one matches the file's own formatting

    @property
    def reference(self) -> str | None:
        return self.fields.get("Reference")


def parse_schematic(path: Path) -> list[Symbol]:
    return parse_schematic_text(path.read_text())


def parse_schematic_text(text: str) -> list[Symbol]:
    root = parse(text)
    return [_read_symbol(node, text) for node in root.all("symbol")]


def _read_symbol(node: Node, text: str) -> Symbol:
    lib_id_node = node.child("lib_id")
    uuid_node = node.child("uuid")
    at_node = node.child("at")
    props = node.all("property")

    fields: dict[str, str] = {}
    field_atoms: dict[str, Atom] = {}
    for prop in props:
        name_atom, value_atom = prop.children[1], prop.children[2]
        fields[name_atom.value] = value_atom.value
        field_atoms[name_atom.value] = value_atom

    if props:
        last_prop = props[-1]
        line_start = text.rfind("\n", 0, last_prop.start) + 1
        last_property_end = last_prop.end
        property_indent = text[line_start:last_prop.start]
    else:
        last_property_end = -1
        property_indent = ""

    return Symbol(
        uuid=uuid_node.children[1].value if uuid_node else "",
        lib_id=lib_id_node.children[1].value if lib_id_node else "",
        at=" ".join(c.value for c in at_node.children[1:]) if at_node else "0 0 0",
        fields=fields,
        field_atoms=field_atoms,
        last_property_end=last_property_end,
        property_indent=property_indent,
    )
