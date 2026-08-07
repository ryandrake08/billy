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
    fields: dict[str, str]        # field name -> current value
    field_atoms: dict[str, Atom]  # field name -> value Atom (byte offsets for later patching)

    @property
    def reference(self) -> str | None:
        return self.fields.get("Reference")


def parse_schematic(path: Path) -> list[Symbol]:
    text = path.read_text()
    root = parse(text)
    return [_read_symbol(node) for node in root.all("symbol")]


def _read_symbol(node: Node) -> Symbol:
    lib_id_node = node.child("lib_id")
    uuid_node = node.child("uuid")
    fields: dict[str, str] = {}
    field_atoms: dict[str, Atom] = {}
    for prop in node.all("property"):
        name_atom, value_atom = prop.children[1], prop.children[2]
        fields[name_atom.value] = value_atom.value
        field_atoms[name_atom.value] = value_atom
    return Symbol(
        uuid=uuid_node.children[1].value if uuid_node else "",
        lib_id=lib_id_node.children[1].value if lib_id_node else "",
        fields=fields,
        field_atoms=field_atoms,
    )
