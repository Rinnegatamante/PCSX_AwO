#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Upstream pin state machines copyright smf.
"""Extract BSD MAME X76F041/X76F100 pin state machines without device framework."""
from pathlib import Path
import re
import sys

def main():
    root=Path(__file__).resolve().parent;upstream=Path(sys.argv[1])
    output='// SPDX-License-Identifier: BSD-3-Clause\n// copyright-holders:smf\n// MAME X76 secure flash pin state machines; generated, do not edit.\n'
    for number in ('041','100'):
        base='x76f'+number;name='X76F'+number
        header=(upstream/(base+'.h')).read_text();source=(upstream/(base+'.cpp')).read_text()
        enums=header[header.index('\tenum '):header.index('\toptional_memory_region')]
        members=header[header.index('\t// internal state'):header.index('\n};')]
        output+=f'struct {name} {{\nvoid verboselog(int,const char*,...) {{}}\nvoid device_reset();\nvoid write_cs(int);\nvoid write_rst(int);\nvoid write_scl(int);\nvoid write_sda(int);\nint read_sda();\nuint8_t *password();\nvoid password_ok();\nint data_offset();\n'
        if number=='041':output+='void load_address();\n'
        output+=enums+members+'\n};\n'
        functions=source[source.index('void '+base+'_device::device_reset()'):source.index('void '+base+'_device::nvram_default()')]
        functions=functions.replace(base+'_device::',name+'::')
        # Reset the data cursor after authentication; malformed hosts may omit repeated START.
        functions=functions.replace('void '+name+'::password_ok()\n{','void '+name+'::password_ok()\n{\n\tm_byte = 0; // Separate password-byte cursor from authenticated data cursor.')
        # Program-only permission is bitwise, not a numeric magnitude comparison.
        functions=functions.replace('m_write_buffer[m_byte] < m_data[offset]', '(m_write_buffer[m_byte] & m_data[offset]) != m_data[offset]')
        output+=functions+'\n'
    root.joinpath('konami_security_x76.inc').write_text(output)
if __name__=='__main__':main()
