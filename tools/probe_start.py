"""Reset device, capture COM9 boot logs, then send one FW_START and
report both the UDP reply and the COM9 output around it."""
import serial
import socket
import struct
import subprocess
import sys
import threading
import time

STLINK = (r"C:\Program Files (x86)\STMicroelectronics\STM32 ST-LINK Utility"
          r"\ST-LINK Utility\ST-LINK_CLI.exe")

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

subprocess.run([STLINK, '-c', 'SWD', 'SWCLK=4000', '-Rst'],
               capture_output=True)
time.sleep(12)  # boot + net bring-up window

keyhash = bytes.fromhex(
    '8960ea405416c90ca03cc0f167ef17022ea5a97389f9ed74159700734e32bd8c')
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(('192.168.12.150', 0))
s.settimeout(5)
pkt = bytes([0x01]) + struct.pack('<I', 310740) + keyhash
mark = len(collected)
try:
    s.sendto(pkt, ('192.168.12.101', 8600))
    r, addr = s.recvfrom(1024)
    print('START reply:', r.hex())
except socket.timeout:
    print('START timeout')
time.sleep(2)
stop = True
t.join(timeout=3)
txt = collected.decode('ascii', errors='replace')
print('=== COM9 boot ===')
print(txt[:2500])
print('=== COM9 around START ===')
print(txt[mark:mark + 1200])
