"""Generate a C byte-array header from a gzip-compressed file.

Usage: gen_gz.py <input> <output.h> <macro_prefix>
Emits:  #define <PREFIX>_SIZE N  followed by hex bytes (for use inside
an array initializer). Deterministic output (mtime=0) so rebuilds are
stable.
"""
import gzip
import sys

inp, out, prefix = sys.argv[1], sys.argv[2], sys.argv[3]

data = open(inp, "rb").read()
gz = gzip.compress(data, 9, mtime=0)

with open(out, "w", newline="\n") as f:
    f.write("/* generated from %s by tools/gen_gz.py - do not edit */\n" % inp)
    f.write("#define %s_SIZE %d\n" % (prefix, len(gz)))
    for i in range(0, len(gz), 16):
        f.write(",".join("0x%02x" % b for b in gz[i : i + 16]) + ",\n")
