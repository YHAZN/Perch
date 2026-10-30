"""Validate HID report structure against the bytes sent by the remote transport."""
import re
from pathlib import Path

source = (Path(__file__).resolve().parents[1] / 'src/remote.cpp').read_text()
body = source.split('REPORT_MAP[] = {', 1)[1].split('};', 1)[0]
body = re.sub(r'//[^\n]*', '', body)
data = bytes(int(v, 16) for v in re.findall(r'0x([0-9a-fA-F]+)', body))
state = {'id': 0, 'size': 0, 'count': 0, 'page': 0, 'minimum': 0, 'maximum': 0}
bits, fields, usages = {}, [], []
depth = 0
i = 0
while i < len(data):
    prefix = data[i]
    i += 1
    assert prefix != 0xFE, 'Unexpected long HID item'
    size = (0, 1, 2, 4)[prefix & 3]
    raw = data[i:i+size]
    assert len(raw) == size
    i += size
    value = int.from_bytes(raw, 'little')
    kind, tag = (prefix >> 2) & 3, prefix >> 4
    if kind == 1:
        key = {0: 'page', 1: 'minimum', 2: 'maximum', 7: 'size', 8: 'id', 9: 'count'}.get(tag)
        if key:
            state[key] = int.from_bytes(raw, 'little', signed=True) if tag == 1 else value
    elif kind == 2 and tag == 0:
        usages.append(value)
    elif kind == 0:
        if tag == 10:
            depth += 1
        elif tag == 12:
            depth -= 1
            assert depth >= 0
        elif tag == 8:
            assert state['id'] and state['size'] and state['count']
            bits[state['id']] = bits.get(state['id'], 0) + state['size'] * state['count']
            fields.append({**state, 'flags': value, 'usages': tuple(usages)})
        usages = []

assert depth == 0, 'Unclosed HID collection'
assert bits == {1: 64, 2: 16, 3: 32}, bits
axes = next(f for f in fields if f['id'] == 3 and f['page'] == 1)
assert axes['usages'] == (0x30, 0x31, 0x38), axes
assert (axes['size'], axes['count'], axes['minimum'], axes['maximum'], axes['flags']) == (8, 3, -127, 127, 6)
buttons = next(f for f in fields if f['id'] == 3 and f['page'] == 9 and not f['flags'] & 1)
assert (buttons['size'], buttons['count']) == (1, 3)
print('PASS: keyboard 8 bytes, consumer 2 bytes, mouse 4 bytes; balanced collections; signed relative X/Y/wheel; 3 buttons')
