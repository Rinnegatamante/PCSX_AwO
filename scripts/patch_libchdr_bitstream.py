#!/usr/bin/env python3
"""Build-local unsigned-shift fixes for the pinned libchdr bitstream reader."""
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text()
changes = {
    '#include "../include/libchdr/bitstream.h"': '#include <libchdr/bitstream.h>',
    'bitstream->read[bitstream->doffset] << (24 - bitstream->bits)':
        '(uint32_t)bitstream->read[bitstream->doffset] << (24 - bitstream->bits)',
    'bitstream->buffer <<= numbits;':
        'bitstream->buffer = numbits == 32 ? 0 : bitstream->buffer << numbits;',
}
for old, new in changes.items():
    if source.count(old) != 1:
        raise SystemExit(f"libchdr bitstream source changed; expected exactly one: {old}")
    source = source.replace(old, new)
Path(sys.argv[2]).write_text(source)
