#!/usr/bin/env python3
"""Embed the pinned System 11 profile registry for the native C loader."""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
document = json.loads((ROOT / 'scripts/system11_boards.json').read_text())
assert document['mame_source_commit'] == 'deb2f54fe7300e7a192690318beb860159bfd52a'
assert len(document['profiles']) == 36
lines = ['/* Generated from scripts/system11_boards.json; do not edit. */']
for row in document['profiles']:
    values = [row['set'], row.get('parent') or 'none', row['model'],
              row['banking'], row['keycus'], 'true' if row['lightgun'] else 'false',
              row['machine'], row['input'], row['flags']]
    lines.append('{' + ', '.join(json.dumps(v) for v in values) + '},')
(ROOT / 'libpcsxcore/arcade/system11_profiles.inc').write_text('\n'.join(lines) + '\n')
