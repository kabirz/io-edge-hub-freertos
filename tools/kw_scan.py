"""Keyword scan helper: python tools/kw_scan.py <file> kw1 kw2 ..."""
import sys

s = open(sys.argv[1], encoding='utf-8', errors='replace').read()
print('size:', len(s))
for kw in sys.argv[2:]:
    print('%-14s: %d' % (kw, s.count(kw)))
