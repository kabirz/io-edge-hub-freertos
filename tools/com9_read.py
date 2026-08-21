"""Reset via ST-LINK and capture COM9 for N seconds."""
import serial
import subprocess
import sys
import time

STLINK = r"C:\Program Files (x86)\STMicroelectronics\STM32 ST-LINK Utility\ST-LINK Utility\ST-LINK_CLI.exe"
SECS = int(sys.argv[1]) if len(sys.argv) > 1 else 10
DO_RESET = '--no-rst' not in sys.argv

if DO_RESET:
    subprocess.run([STLINK, '-c', 'SWD', 'SWCLK=4000', '-Rst'],
                   capture_output=True)

p = serial.Serial('COM9', 115200, timeout=0.2)
buf = bytearray()
t0 = time.time()
while time.time() - t0 < SECS:
    n = p.in_waiting
    if n:
        buf.extend(p.read(n))
    time.sleep(0.02)
p.close()
print('--- %d bytes ---' % len(buf))
print(buf.decode('ascii', errors='replace')[-4000:])
