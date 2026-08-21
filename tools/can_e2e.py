"""CAN end-to-end upgrade with COM9 capture (swap proof via boot log)."""
import serial
import subprocess
import sys
import threading
import time

ROOT = r'C:\Users\jxwaz\code\io-edge-hub-freertos'
collected = bytearray()
stop = False


def reader():
    try:
        p = serial.Serial('COM9', 115200, timeout=0.2)
        while not stop:
            n = p.in_waiting
            if n:
                collected.extend(p.read(n))
            time.sleep(0.02)
        p.close()
    except Exception as e:
        print('COM9 err:', e)


t = threading.Thread(target=reader, daemon=True)
t.start()
time.sleep(0.5)

r = subprocess.run(
    [sys.executable, ROOT + r'\tools\firmware_upgrade_can.py',
     'upgrade', '-f', ROOT + r'\build\test-v0.3.11.bin'],
    capture_output=True)
print((r.stdout + r.stderr).decode('utf-8', errors='replace'))
time.sleep(5)
stop = True
t.join(timeout=3)
txt = collected.decode('ascii', errors='replace')
print('=== COM9 ===')
print(txt[-2200:])
