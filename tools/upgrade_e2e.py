"""End-to-end UDP upgrade test with COM9 capture: upgrade -> reboot ->
swap -> new version, all logged."""
import serial
import subprocess
import sys
import threading
import time

ROOT = r'C:\Users\jxwaz\code\io-edge-hub-freertos'
collected = bytearray()
done = False


def reader():
    global done
    try:
        p = serial.Serial('COM9', 115200, timeout=0.5)
        while not done:
            n = p.in_waiting
            if n:
                collected.extend(p.read(n))
            time.sleep(0.05)
        p.close()
    except Exception as e:
        print('COM9 err:', e)


t = threading.Thread(target=reader, daemon=True)
t.start()
time.sleep(0.5)

r = subprocess.run(
    [sys.executable, ROOT + r'\tools\firmware_upgrade.py',
     '--ip', '192.168.12.101', '--src', '192.168.12.150',
     '-f', ROOT + r'\build\test-v0.3.9.bin'],
    capture_output=True)
out = (r.stdout + r.stderr).decode('utf-8', errors='replace')
print(out)

time.sleep(40)  # swap (~1.3MB over SPI) + reboot window
done = True
t.join(timeout=3)
print('=== COM9 tail ===')
print(collected.decode('ascii', errors='replace')[-2500:])
