#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Embed build-generated SPIR-V; no runtime shader compiler or shader path."""
import pathlib
import struct
import sys
with open(sys.argv[1], 'w') as out:
    out.write('/* Generated from MIT-licensed in-tree GLSL. */\n')
    for path in sys.argv[2:]:
        data = pathlib.Path(path).read_bytes()
        words = struct.unpack('<' + 'I' * (len(data) // 4), data)
        name = pathlib.Path(path).stem
        out.write('static const uint32_t %s_spv[] = {\n' % name)
        for i in range(0, len(words), 8):
            out.write(','.join('0x%08xu' % w for w in words[i:i+8]) + ',\n')
        out.write('};\n')
