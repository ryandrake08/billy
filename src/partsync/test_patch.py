#!/usr/bin/env python3
"""Unit tests for in-place field patching. No pytest dependency — run directly:

    python3 test_patch.py
"""
import tempfile
from pathlib import Path

from patch import (
    SchematicLockedError,
    UnknownFieldError,
    apply_field_updates,
    escape_value,
    lock_path,
    patch_field,
    write_field,
)
from schematic import parse_schematic_text

FIXTURE = r"""
(kicad_sch
	(version 20231120)
	(generator "eeschema")
	(symbol
		(lib_id "Device:C")
		(at 100 100 0)
		(uuid "uuid-c1")
		(property "Reference" "C1"
			(at 100 98 0)
		)
		(property "Value" "220p"
			(at 100 102 0)
		)
		(property "MF" "OldMF"
			(at 100 100 0)
		)
		(property "Description" "plain text"
			(at 100 100 0)
		)
	)
	(symbol
		(lib_id "Device:R")
		(at 200 200 0)
		(uuid "uuid-r1")
		(property "Reference" "R1"
			(at 200 198 0)
		)
		(property "Value" "10k"
			(at 200 202 0)
		)
		(property "MF" "AnotherMF"
			(at 200 200 0)
		)
	)
)
"""


def check(got, want, label):
    assert got == want, f"{label}: expected {want!r}, got {got!r}"


def _fixture_path(tmp_path: Path) -> Path:
    p = tmp_path / "fixture.kicad_sch"
    p.write_text(FIXTURE)
    return p


def test_escape_value_round_trips_quotes_and_backslashes():
    raw = 'has "quotes" and a \\backslash'
    escaped = escape_value(raw)
    check(escaped, r'"has \"quotes\" and a \\backslash"', "escaped form")
    # feed it back through the real tokenizer, not just eyeball the string
    reparsed = parse_schematic_text(f'(kicad_sch (symbol (uuid "x") (property "Description" {escaped})))')
    check(reparsed[0].fields["Description"], raw, "unescapes back to the original")


def test_apply_field_updates_no_changes_is_byte_identical():
    symbols = parse_schematic_text(FIXTURE)
    out = apply_field_updates(FIXTURE, symbols, {})
    check(out, FIXTURE, "empty update set leaves text untouched")


def test_apply_field_updates_changes_only_target_span():
    symbols = parse_schematic_text(FIXTURE)
    c1 = next(s for s in symbols if s.reference == "C1")
    out = apply_field_updates(FIXTURE, symbols, {c1.uuid: {"MF": "NewMF"}})

    check("NewMF" in out, True, "new value present")
    check("OldMF" in out, False, "old value gone")
    check('"AnotherMF"' in out, True, "unrelated symbol's field untouched")

    # every line except the one containing the changed value is identical
    before_lines = FIXTURE.splitlines()
    after_lines = out.splitlines()
    check(len(before_lines), len(after_lines), "line count unchanged (single-token replacement)")
    changed = [i for i, (a, b) in enumerate(zip(before_lines, after_lines)) if a != b]
    check(len(changed), 1, "exactly one line differs")
    check("NewMF" in after_lines[changed[0]], True, "the differing line is the MF value line")


def test_unknown_field_raises_by_default():
    symbols = parse_schematic_text(FIXTURE)
    c1 = next(s for s in symbols if s.reference == "C1")
    try:
        apply_field_updates(FIXTURE, symbols, {c1.uuid: {"LCSC Part": "C999"}})
        raise AssertionError("expected UnknownFieldError")
    except UnknownFieldError:
        pass


def test_create_inserts_new_property_matching_kicad_format():
    symbols = parse_schematic_text(FIXTURE)
    c1 = next(s for s in symbols if s.reference == "C1")
    out = apply_field_updates(FIXTURE, symbols, {c1.uuid: {"Mouser Part": "MOUSER123"}}, create=True)

    expected_block = (
        '\t\t(property "Mouser Part" "MOUSER123"\n'
        '\t\t\t(at 100 100 0)\n'
        '\t\t\t(hide yes)\n'
        '\t\t\t(show_name no)\n'
        '\t\t\t(do_not_autoplace no)\n'
        '\t\t\t(effects\n'
        '\t\t\t\t(font\n'
        '\t\t\t\t\t(size 1.27 1.27)\n'
        '\t\t\t\t)\n'
        '\t\t\t)\n'
        '\t\t)'
    )
    check(expected_block in out, True, "new block matches the hidden-field format existing properties use")

    reparsed = parse_schematic_text(out)
    c1_after = next(s for s in reparsed if s.reference == "C1")
    check(c1_after.fields["Mouser Part"], "MOUSER123", "round trips through the real parser")
    r1_after = next(s for s in reparsed if s.reference == "R1")
    check("Mouser Part" in r1_after.fields, False, "unrelated symbol unaffected")


def test_create_multiple_fields_appends_in_call_order():
    symbols = parse_schematic_text(FIXTURE)
    c1 = next(s for s in symbols if s.reference == "C1")
    out = apply_field_updates(
        FIXTURE, symbols, {c1.uuid: {"Mouser Part": "MP1", "Mouser MF": "MF1"}}, create=True
    )
    idx_desc = out.index('"Description"')
    idx_part = out.index('"Mouser Part"')
    idx_mf = out.index('"Mouser MF"')
    check(idx_desc < idx_part < idx_mf, True, "new properties land after existing ones, in the order given")


def test_create_on_symbol_with_no_properties_raises():
    text = '(kicad_sch (symbol (lib_id "Device:C") (at 0 0 0) (uuid "bare")))'
    symbols = parse_schematic_text(text)
    try:
        apply_field_updates(text, symbols, {"bare": {"Mouser Part": "x"}}, create=True)
        raise AssertionError("expected UnknownFieldError")
    except UnknownFieldError:
        pass


def test_patch_field_round_trip_via_disk():
    with tempfile.TemporaryDirectory() as d:
        path = _fixture_path(Path(d))
        old_text, new_text = patch_field(path, "C1", "MF", "NewMF")
        check(old_text, FIXTURE, "old_text matches what's on disk")
        check("NewMF" in new_text, True, "new_text has the patched value")
        check(path.read_text(), FIXTURE, "patch_field is read-only, disk untouched")


def test_write_field_writes_and_is_idempotent():
    with tempfile.TemporaryDirectory() as d:
        path = _fixture_path(Path(d))
        write_field(path, "C1", "MF", "NewMF")
        check(parse_schematic_text(path.read_text())[0].fields["MF"], "NewMF", "value persisted to disk")

        # re-running with the same value is a no-op (idempotent, no spurious rewrite)
        before = path.read_text()
        write_field(path, "C1", "MF", "NewMF")
        check(path.read_text(), before, "second write with same value changes nothing")


def test_write_field_refuses_when_locked():
    with tempfile.TemporaryDirectory() as d:
        path = _fixture_path(Path(d))
        lock_path(path).touch()
        try:
            write_field(path, "C1", "MF", "NewMF")
            raise AssertionError("expected SchematicLockedError")
        except SchematicLockedError:
            pass
        check(path.read_text(), FIXTURE, "file untouched when locked")


def test_write_field_force_overrides_lock():
    with tempfile.TemporaryDirectory() as d:
        path = _fixture_path(Path(d))
        lock_path(path).touch()
        write_field(path, "C1", "MF", "NewMF", force=True)
        check(parse_schematic_text(path.read_text())[0].fields["MF"], "NewMF", "force bypasses the lock check")


def test_write_field_create_persists_new_property_to_disk():
    with tempfile.TemporaryDirectory() as d:
        path = _fixture_path(Path(d))
        write_field(path, "C1", "Mouser Part", "MOUSER123", create=True)
        symbols = parse_schematic_text(path.read_text())
        c1 = next(s for s in symbols if s.reference == "C1")
        check(c1.fields["Mouser Part"], "MOUSER123", "new property persisted and re-parses correctly")

        # without --create, the same call on a genuinely missing field still refuses
        try:
            write_field(path, "R1", "Mouser Part", "x")
            raise AssertionError("expected UnknownFieldError")
        except UnknownFieldError:
            pass


if __name__ == "__main__":
    test_escape_value_round_trips_quotes_and_backslashes()
    test_apply_field_updates_no_changes_is_byte_identical()
    test_apply_field_updates_changes_only_target_span()
    test_unknown_field_raises_by_default()
    test_create_inserts_new_property_matching_kicad_format()
    test_create_multiple_fields_appends_in_call_order()
    test_create_on_symbol_with_no_properties_raises()
    test_patch_field_round_trip_via_disk()
    test_write_field_writes_and_is_idempotent()
    test_write_field_refuses_when_locked()
    test_write_field_force_overrides_lock()
    test_write_field_create_persists_new_property_to_disk()
    print("ok — all patch tests passed")
