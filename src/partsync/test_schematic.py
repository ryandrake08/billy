#!/usr/bin/env python3
"""Unit tests for schematic symbol/field extraction. No pytest dependency — run directly:

    python3 test_schematic.py
"""
import tempfile
from pathlib import Path

from schematic import parse_schematic

# A trimmed-down but structurally real .kicad_sch: a lib_symbols cache block (whose nested
# "symbol" nodes must NOT be read as instances) plus three placed symbols exercising the cases
# the parser needs to handle: a fully-fielded part, a part with no LCSC Part (must be skipped),
# and a part missing some fields entirely (must read back as "").
FIXTURE = r"""
(kicad_sch
	(version 20231120)
	(generator "eeschema")
	(lib_symbols
		(symbol "Device:C"
			(property "Reference" "C"
				(at 0.635 2.54 0)
			)
			(property "Value" "C"
				(at 0.635 -2.54 0)
			)
			(symbol "C_0_1"
				(pin passive line
					(at 0 3.81 270)
				)
			)
		)
	)
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
		(property "LCSC Part" "C1530"
			(at 100 100 0)
		)
		(property "MF" "Samsung"
			(at 100 100 0)
		)
		(property "MP" "CL05A221JB5NNNC"
			(at 100 100 0)
		)
		(property "Description" "has \"quoted\" text"
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
		(property "LCSC Part" ""
			(at 200 200 0)
		)
	)
	(symbol
		(lib_id "Device:C")
		(at 300 300 0)
		(uuid "uuid-c2")
		(property "Reference" "C2"
			(at 300 298 0)
		)
		(property "Value" "100n"
			(at 300 302 0)
		)
		(property "LCSC Part" "C123"
			(at 300 300 0)
		)
	)
)
"""


def check(got, want, label):
    assert got == want, f"{label}: expected {want!r}, got {got!r}"


def _write_fixture(tmp_path: Path) -> Path:
    p = tmp_path / "fixture.kicad_sch"
    p.write_text(FIXTURE)
    return p


def test_reads_all_placed_symbols_not_lib_cache():
    with tempfile.TemporaryDirectory() as d:
        symbols = parse_schematic(_write_fixture(Path(d)))
    check([s.reference for s in symbols], ["C1", "R1", "C2"], "placed symbols only, cache excluded")


def test_fully_fielded_symbol_reads_correctly():
    with tempfile.TemporaryDirectory() as d:
        symbols = parse_schematic(_write_fixture(Path(d)))
    c1 = next(s for s in symbols if s.reference == "C1")
    check(c1.uuid, "uuid-c1", "uuid")
    check(c1.lib_id, "Device:C", "lib_id")
    check(c1.fields["LCSC Part"], "C1530", "LCSC Part")
    check(c1.fields["MF"], "Samsung", "MF")
    check(c1.fields["MP"], "CL05A221JB5NNNC", "MP")
    check(c1.fields["Description"], 'has "quoted" text', "Description (unescaped)")


def test_missing_field_reads_as_absent_not_error():
    with tempfile.TemporaryDirectory() as d:
        symbols = parse_schematic(_write_fixture(Path(d)))
    c2 = next(s for s in symbols if s.reference == "C2")
    check(c2.fields.get("MF", ""), "", "MF absent on C2 reads as empty, not KeyError")
    check(c2.fields.get("Description", ""), "", "Description absent on C2 reads as empty")


def test_empty_lcsc_part_is_falsy_for_report_filtering():
    with tempfile.TemporaryDirectory() as d:
        symbols = parse_schematic(_write_fixture(Path(d)))
    r1 = next(s for s in symbols if s.reference == "R1")
    check(bool(r1.fields.get("LCSC Part")), False, "empty LCSC Part should be filtered out by report")


if __name__ == "__main__":
    test_reads_all_placed_symbols_not_lib_cache()
    test_fully_fielded_symbol_reads_correctly()
    test_missing_field_reads_as_absent_not_error()
    test_empty_lcsc_part_is_falsy_for_report_filtering()
    print("ok — all schematic tests passed")
