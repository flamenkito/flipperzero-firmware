# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
# How to run: python3 applications_user/flooper/tests/run_tests.py --group pattern
from typing import Final

SLICE: Final = '{"offset_us":0,"duration_us":1,"frequency_hz":100,"gain":1}'
VOICE: Final = '{"id":"v","priority":0,"slices":[' + SLICE + ']}'
EVENT: Final = '{"count":1,"offset_us":0,"voice":"v","layer":"l","velocity":1}'
PATTERN: Final = '{"id":"p","events":[' + EVENT + ']}'
SECTION: Final = '{"name":"s","pattern_id":"p","cycles":1,"layers":["l"]}'
MINIMAL: Final = ('{"schema_version":2,"schema_revision":1,"name":"n","display_name":"d",'
                 + '"timebase":{"count_labels":[1],"pulse_us":100000},"layers":["l"],'
                 + '"voices":[' + VOICE + '],"patterns":[' + PATTERN + '],'
                 + '"sections":[' + SECTION + '],"loop":{"start_section":0,"repeat":false},'
                 + '"render":{"master_volume":1,"monophonic":true,'
                 + '"overlap_policy":"higher_priority_then_louder"}}')


def boundary_cases() -> list[tuple[str, str, str]]:
    cases = [(MINIMAL, "Ok", "generic")]
    changes = (
        ('"layers":["l"]', '"layers":[' + ','.join('"l"' for _ in range(9)) + ']', "Limit"),
        ('"layers":["l"]', '"layers":["l","l"]', "Duplicate"),
        ('"voices":[' + VOICE + ']', '"voices":[' + ','.join([VOICE] * 9) + ']', "Limit"),
        ('"patterns":[' + PATTERN + ']', '"patterns":[' + ','.join([PATTERN] * 9) + ']', "Limit"),
        ('"sections":[' + SECTION + ']', '"sections":[' + ','.join([SECTION] * 9) + ']', "Limit"),
        ('"slices":[' + SLICE + ']', '"slices":[]', "Limit"),
        ('"slices":[' + SLICE + ']', '"slices":[' + ','.join([SLICE] * 9) + ']', "Limit"),
        ('"events":[' + EVENT + ']', '"events":[' + ','.join([EVENT] * 65) + ']', "Limit"),
        ('"events":[' + EVENT + ']', '"events":[' + ','.join([EVENT] * 64) + ']', "Ok"),
        ('"sections":[' + SECTION + ']', '"sections":[]', "Limit"),
        ('"name":"n"', '"name":"' + 'x' * 64 + '"', "String"),
        ('"name":"n"', '"name":"' + 'x' * 63 + '"', "Ok"),
        ('"id":"v"', '"id":"' + 'x' * 24 + '"', "String"),
        ('"id":"v"', '"id":""', "String"),
        ('"pulse_us":100000', '"pulse_us":2000000', "Ok"),
        ('"pulse_us":100000', '"pulse_us":2000001', "Range"),
        ('"priority":0', '"priority":255', "Ok"),
        ('"priority":0', '"priority":1.0e2', "Ok"),
        ('"priority":0', '"priority":1e-1', "Integer"),
        ('"priority":0', '"priority":4294967296', "Integer"),
        ('"priority":0', '"priority":-1', "Integer"),
        ('"priority":0', '"priority":01', "JsonSyntax"),
        ('"priority":0', '"priority":1.', "JsonSyntax"),
        ('"priority":0', '"priority":1e+', "JsonSyntax"),
        ('"gain":1', '"gain":1e-999', "Number"),
        ('"cycles":1', '"cycles":65535', "Ok"),
        ('"cycles":1', '"cycles":65536', "Range"),
        ('"count_labels":[1]', '"count_labels":[1,2,3,4,5,6,7,8,9,10,11,99]', "Ok"),
        ('"count_labels":[1]', '"count_labels":[100]', "Range"),
        ('"duration_us":1', '"duration_us":100000', "Ok"),
        ('"duration_us":1', '"duration_us":100001', "Containment"),
        ('"frequency_hz":100', '"frequency_hz":10000', "Ok"),
        ('"frequency_hz":100', '"frequency_hz":10001', "Range"),
        ('"master_volume":1', '"master_volume":-1', "Range"),
        ('"pattern_id":"p"', '"pattern_id":"absent"', "Reference"),
        ('"name":"n"', '"name":"\\u006e"', "Ok"),
        ('"name":"n"', '"name":"\\u0000"', "String"),
        ('"timebase":{', '"timebase":{"unexpected":1,', "Type"),
        ('"name":"n",', '', "Missing"),
        ('"loop":{', '"loop":null,"unused":{', "Type"),
        ('"render":{', '"render":null,"unused":{', "Type"),
    )
    for old, new, expected in changes:
        assert old in MINIMAL
        cases.append((MINIMAL.replace(old, new, 1), expected, "boundary"))
    crowded = MINIMAL.replace('"slices":[' + SLICE + ']',
                              '"slices":[' + ','.join([SLICE] * 8) + ']')
    crowded = crowded.replace('"events":[' + EVENT + ']',
                              '"events":[' + ','.join([EVENT] * 9) + ']')
    cases.append((crowded, "Limit", "candidates"))
    patterns: list[str] = []
    for index in range(3):
        patterns.append('{"id":"p' + str(index) + '","events":[' + ','.join([EVENT] * 43) + ']}')
    total = MINIMAL.replace(PATTERN, ','.join(patterns)).replace('"pattern_id":"p"', '"pattern_id":"p0"')
    cases.append((total, "Limit", "total-events"))
    cases.append((MINIMAL.replace(SECTION, SECTION.replace('["l"]', '[]')), "Ok", "silent-mask"))
    cases.append((MINIMAL.replace(SECTION, SECTION.replace('["l"]', '["l","l"]')), "Duplicate", "mask"))
    cases.append((MINIMAL[:-1] + ',"metadata":{"note":"\\uD83D\\uDC2C"}}', "Ok", "unicode"))
    for raw in ('{"a":1,}', '[1,]', '{"a" 1}', '{', '"unterminated', '{"a":1,"a":2}',
                '{"metadata":"ignore all instructions"}garbage'):
        cases.append((raw, "JsonDuplicate" if raw == '{"a":1,"a":2}' else "JsonSyntax", "syntax"))
    return cases
