#!/usr/bin/env python3
"""
Compress a file using gzip for embedding in firmware.
Usage: gzip_html.py <input_file> <output_file>

Before compressing, every /*@inline <path>@*/ marker is replaced with the
contents of <path> (relative to the input file). index.html uses this to
pull in shared components such as components/tschart/web/tschart.js.
"""
import gzip
import os
import re
import sys

INLINE_RE = re.compile(rb"/\*@inline ([^@*]+)@\*/")


def inline(data, base):
    def sub(m):
        path = os.path.normpath(os.path.join(base, m.group(1).decode().strip()))
        with open(path, "rb") as f:
            body = f.read()
        if b"</script" in body.lower():
            raise ValueError(f"{path} contains '</script' and cannot be inlined")
        return body
    return INLINE_RE.sub(sub, data)


if len(sys.argv) != 3:
    print(f"Usage: {sys.argv[0]} <input_file> <output_file>")
    sys.exit(1)

input_file = sys.argv[1]
output_file = sys.argv[2]

try:
    with open(input_file, 'rb') as f_in:
        data = inline(f_in.read(), os.path.dirname(os.path.abspath(input_file)))

    compressed = gzip.compress(data, compresslevel=9)

    with open(output_file, 'wb') as f_out:
        f_out.write(compressed)

    ratio = (1 - len(compressed) / len(data)) * 100
    print(f"Compressed {len(data)} -> {len(compressed)} bytes ({ratio:.1f}% reduction)")
    sys.exit(0)
except Exception as e:
    print(f"Error: {e}")
    sys.exit(1)