import socket
import struct
import time

IP = '192.168.12.101'
UDP_PORT = 8600

CMD_GET_VERSION = 0x04
CMD_REBOOT = 0x05
CMD_GET_IP = 0x11

print("=== UDP Commands Test (Zephyr-compatible codes) ===\n")

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(3)

# Test 1: GET_VERSION (0x04)
print("--- GET_VERSION (0x04) ---")
s.sendto(bytes([CMD_GET_VERSION]), (IP, UDP_PORT))
try:
    resp, addr = s.recvfrom(256)
    print(f"  Raw hex: {resp.hex()}")
    print(f"  Raw text: {resp.decode('ascii', errors='replace')}")
    if resp[0] == CMD_GET_VERSION:
        version = resp[1:].decode('ascii', errors='replace')
        print(f"  Version string: {version}")
except socket.timeout:
    print("  Timeout")

# Test 2: GET_IP sanity
print("\n--- GET_IP (0x11) ---")
s.sendto(bytes([CMD_GET_IP]), (IP, UDP_PORT))
try:
    resp2, _ = s.recvfrom(256)
    if len(resp2) >= 5:
        ip = '.'.join(str(b) for b in resp2[1:5])
        print(f"  IP: {ip}")
except socket.timeout:
    print("  Timeout")

# Test 3: REBOOT (0x05) - immediate
print("\n--- REBOOT (0x05) ---")
s.sendto(bytes([CMD_REBOOT]), (IP, UDP_PORT))
try:
    resp3, _ = s.recvfrom(256)
    print(f"  Raw: {resp3.hex()} ({len(resp3)} bytes)")
    if len(resp3) >= 2:
        print(f"  cmd=0x{resp3[0]:02X} ok={resp3[1]}")
except socket.timeout:
    print("  Timeout")

s.close()

# Wait for reboot and verify
print("\n--- Waiting for reboot... ---")
time.sleep(5)

s2 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s2.settimeout(3)
s2.sendto(bytes([CMD_GET_IP]), (IP, UDP_PORT))
try:
    resp4, _ = s2.recvfrom(256)
    if len(resp4) >= 5:
        ip = '.'.join(str(b) for b in resp4[1:5])
        print(f"  Device back online! IP: {ip}")
except socket.timeout:
    print("  Device still rebooting")
s2.close()

print("\n=== Tests complete ===")
