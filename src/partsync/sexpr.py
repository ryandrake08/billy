"""Minimal S-expression parser for KiCad's .kicad_sch file format.

Tracks byte offsets on every atom so a value can be patched in place later
without touching anything else the file contains.
"""
from __future__ import annotations

from dataclasses import dataclass


@dataclass
class Atom:
    value: str    # unescaped text
    quoted: bool
    start: int    # offset of the first byte (opening quote, if any)
    end: int      # offset just past the last byte (closing quote, if any)


@dataclass
class Node:
    children: list["Node | Atom"]
    start: int
    end: int

    @property
    def head(self) -> str | None:
        first = self.children[0] if self.children else None
        return first.value if isinstance(first, Atom) else None

    def child(self, tag: str) -> "Node | None":
        for c in self.children:
            if isinstance(c, Node) and c.head == tag:
                return c
        return None

    def all(self, tag: str) -> list["Node"]:
        return [c for c in self.children if isinstance(c, Node) and c.head == tag]


def parse(text: str) -> Node:
    node, idx = _parse_list(text, _skip_ws(text, 0))
    idx = _skip_ws(text, idx)
    if idx != len(text):
        raise ValueError(f"unexpected trailing content at offset {idx}")
    return node


def _skip_ws(text: str, idx: int) -> int:
    while idx < len(text) and text[idx].isspace():
        idx += 1
    return idx


def _parse_list(text: str, idx: int) -> tuple[Node, int]:
    start = idx
    assert text[idx] == "(", f"expected '(' at offset {idx}"
    idx += 1
    children: list[Node | Atom] = []
    while True:
        idx = _skip_ws(text, idx)
        if idx >= len(text):
            raise ValueError(f"unterminated list starting at offset {start}")
        if text[idx] == ")":
            idx += 1
            return Node(children, start, idx), idx
        if text[idx] == "(":
            child, idx = _parse_list(text, idx)
        else:
            child, idx = _parse_atom(text, idx)
        children.append(child)


def _parse_atom(text: str, idx: int) -> tuple[Atom, int]:
    start = idx
    if text[idx] == '"':
        idx += 1
        chars = []
        while text[idx] != '"':
            if text[idx] == "\\":
                idx += 1
            chars.append(text[idx])
            idx += 1
        idx += 1
        return Atom("".join(chars), True, start, idx), idx
    while idx < len(text) and not text[idx].isspace() and text[idx] not in "()":
        idx += 1
    return Atom(text[start:idx], False, start, idx), idx
