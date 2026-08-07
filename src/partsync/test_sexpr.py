#!/usr/bin/env python3
"""Unit tests for the S-expression parser. No pytest dependency — run directly:

    python3 test_sexpr.py
"""
from sexpr import parse


def check(got, want, label):
    assert got == want, f"{label}: expected {want!r}, got {got!r}"


def test_parse_flat_list():
    root = parse("(a b c)")
    check(root.head, "a", "list head")
    check([c.value for c in root.children], ["a", "b", "c"], "child values")


def test_parse_nested_and_all():
    root = parse("(a (b 1) (b 2) (c 3))")
    bs = root.all("b")
    check(len(bs), 2, "count of 'b' children")
    check([b.children[1].value for b in bs], ["1", "2"], "'b' second values")
    check(root.child("c").children[1].value, "3", "'c' first match")


def test_quoted_string_unescapes():
    root = parse(r'(property "Description" "has \"quotes\" and a \\backslash")')
    check(root.children[2].value, 'has "quotes" and a \\backslash', "unescaped value")


def test_child_missing_returns_none():
    root = parse("(a (b 1))")
    check(root.child("z"), None, "missing child")


def test_atom_offsets_are_exact_spans():
    text = '(property "Description" "has \\"quotes\\" inside")'
    root = parse(text)
    value_atom = root.children[2]
    check(text[value_atom.start:value_atom.end], '"has \\"quotes\\" inside"', "raw source span, quotes included")


def test_offsets_survive_nesting_and_whitespace():
    text = "(symbol\n\t(lib_id \"Device:C\")\n\t(property \"Value\" \"220p\")\n)"
    root = parse(text)
    prop = root.child("property")
    value_atom = prop.children[2]
    check(text[value_atom.start:value_atom.end], '"220p"', "value span inside nested/whitespace-heavy text")


if __name__ == "__main__":
    test_parse_flat_list()
    test_parse_nested_and_all()
    test_quoted_string_unescapes()
    test_child_missing_returns_none()
    test_atom_offsets_are_exact_spans()
    test_offsets_survive_nesting_and_whitespace()
    print("ok — all sexpr tests passed")
