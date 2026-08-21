"""Tiny recursive grep for this CMD-hostile environment.

Usage: python tools/grep.py <root> <pattern> [suffixes]
Prints file:line: text for each match (regex).
"""
import os
import re
import sys

root = sys.argv[1]
pat = re.compile(sys.argv[2])
suffixes = sys.argv[3].split(',') if len(sys.argv) > 3 else ['.c', '.h']

for dirpath, _dirs, files in os.walk(root):
    for f in files:
        if not any(f.endswith(s) for s in suffixes):
            continue
        p = os.path.join(dirpath, f)
        try:
            for i, line in enumerate(open(p, encoding='utf-8', errors='replace')):
                if pat.search(line):
                    print('%s:%d:%s' % (p, i + 1, line.rstrip()))
        except OSError:
            pass
