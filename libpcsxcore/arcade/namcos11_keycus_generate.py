#!/usr/bin/env python3
"""Extract the pinned MAME KEYCUS equations into standalone C functions."""
import argparse
import re
from pathlib import Path

PIN = 'deb2f54fe7300e7a192690318beb860159bfd52a'
IDS = (406, 409, 410, 411, 430, 431, 432, 442, 443)


def generate(source):
    result = ['// license:BSD-3-Clause\n// copyright-holders:smf\n',
              '// Generated from MAME ns11prot.cpp at ' + PIN + '.\n']
    for chip in IDS:
        for kind, return_type, arguments in (
                ('read', 'uint16_t', 'Namcos11Board *board, unsigned offset'),
                ('write', 'void', 'Namcos11Board *board, unsigned offset, uint16_t data')):
            match = re.search(r'\b' + kind + r'\([^)]*\)\s*\{', source[
                source.index('keycus_c%d_device::%s(' % (chip, kind)):])
            start = source.index('keycus_c%d_device::%s(' % (chip, kind)) + match.end()
            depth, end = 1, start
            while depth:
                depth += (source[end] == '{') - (source[end] == '}')
                end += 1
            body = source[start:end - 1]
            body = re.sub(r'logerror\([^;]*;', '', body, flags=re.S)
            body = body.replace('machine().rand()', 'keycus_unknown(board, offset)')
            for index in range(1, 4):
                body = body.replace('m_p%d' % index, 'board->key_p[%d]' % (index - 1))
            if kind == 'write':
                body += '\n    board->status.unknown_keycus_writes++;\n'
            result.append('static %s keycus_%d_%s(%s)\n{%s}\n' %
                          (return_type, chip, kind, arguments, body))
    return '\n'.join(result)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--mame-source', type=Path, required=True)
    parser.add_argument('--output', type=Path,
                        default=Path(__file__).with_name('namcos11_keycus.inc'))
    args = parser.parse_args()
    args.output.write_text(generate((args.mame_source / 'src/mame/namco/ns11prot.cpp').read_text()))
