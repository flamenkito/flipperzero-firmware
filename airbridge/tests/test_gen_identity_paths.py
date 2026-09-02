from pathlib import Path

from airbridge.tools import gen_identity


PRODUCT_ROOT = Path(__file__).resolve().parents[1]
MONOREPO_ROOT = PRODUCT_ROOT.parent


def test_identity_generator_roots_match_merged_layout() -> None:
    # Given the generator's location inside the merged airbridge product tree
    expected = (MONOREPO_ROOT, PRODUCT_ROOT)

    # When its repository roots are imported
    actual = (gen_identity.MONOREPO_ROOT, gen_identity.ROOT)

    # Then firmware inputs resolve from the monorepo and product assets from airbridge
    assert actual == expected


def test_serial_uuids_match_canonical_header_comments() -> None:
    # Given the canonical in-tree firmware UUID header
    header = gen_identity.UUID_HEADER_PATH.read_text(encoding="utf-8")
    expected = tuple(
        match.group(2).casefold() for match in gen_identity.UUID_COMMENT_PATTERN.finditer(header)
    )

    # When the generator parses the controller-order byte arrays
    actual = gen_identity.parse_serial_uuids(gen_identity.UUID_HEADER_PATH)

    # Then it returns the documented on-air UUIDs without changing their order
    assert actual == expected
