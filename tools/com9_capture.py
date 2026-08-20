"""Capture COM9 for N seconds to a file (arg1: seconds, arg2: outfile)."""
import sys
import time

import serial

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 5.0
out = sys.argv[2] if len(sys.argv) > 2 else "com9.log"

s = serial.Serial("COM9", 115200, timeout=0.5)
s.reset_input_buffer()
t0 = time.time()
buf = b""
while time.time() - t0 < secs:
    buf += s.read(4096)
s.close()
with open(out, "wb") as f:
    f.write(buf)
print("captured", len(buf), "bytes ->", out)
