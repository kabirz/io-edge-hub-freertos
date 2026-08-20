import socket
import struct

IP = '192.168.12.101'
UDP_PORT = 8600

# Command codes from udp_cfg.h
CMD_SET_IP      = 0x10
CMD_GET_IP      = 0x11
CMD_SET_MODBUS  = 0x12
CMD_GET_MODBUS  = 0x13
CMD_SET_TIME    = 0x14
CMD_FACTORY_RESET = 0x19

print("=== UDP Config Protocol Tests ===\n")

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(3)

# Test 1: GET_IP (0x11) - no additional data
print("--- GET_IP (0x11) ---")
s.sendto(bytes([CMD_GET_IP]), (IP, UDP_PORT))
try:
    resp, addr = s.recvfrom(256)
    print(f"  Response: {resp.hex()} ({len(resp)} bytes)")
    if len(resp) >= 5 and resp[0] == CMD_GET_IP:
        print(f"  IP: {resp[1]}.{resp[2]}.{resp[3]}.{resp[4]}")
    else:
        print(f"  Unexpected response")
except socket.timeout:
    print("  Timeout")

# Test 2: GET_MODBUS (0x13)
print("\n--- GET_MODBUS (0x13) ---")
s.sendto(bytes([CMD_GET_MODBUS]), (IP, UDP_PORT))
try:
    resp2, _ = s.recvfrom(256)
    print(f"  Response: {resp2.hex()} ({len(resp2)} bytes)")
    if len(resp2) >= 4 and resp2[0] == CMD_GET_MODBUS:
        slave_id = resp2[1]
        baud = struct.unpack('>H', resp2[2:4])[0]
        print(f"  Slave ID: {slave_id}, Baud: {baud}")
except socket.timeout:
    print("  Timeout")

# Test 3: SET_IP (0x10) - set to 192.168.12.101 (same IP)
print("\n--- SET_IP (0x10) ---")
s.sendto(bytes([CMD_SET_IP, 192, 168, 12, 101]), (IP, UDP_PORT))
try:
    resp3, _ = s.recvfrom(256)
    print(f"  Response: {resp3.hex()} ({len(resp3)} bytes)")
    if len(resp3) >= 2:
        print(f"  cmd=0x{resp3[0]:02X} ok={resp3[1]}")
except socket.timeout:
    print("  Timeout")

# Test 4: SET_TIME (0x14) - set to a known time (BE32)
import time as t
now = int(t.time())
print(f"\n--- SET_TIME (0x14) -> {now} ---")
ts_bytes = struct.pack('>I', now)
s.sendto(bytes([CMD_SET_TIME]) + ts_bytes, (IP, UDP_PORT))
try:
    resp4, _ = s.recvfrom(256)
    print(f"  Response: {resp4.hex()} ({len(resp4)} bytes)")
    if len(resp4) >= 2:
        print(f"  cmd=0x{resp4[0]:02X} ok={resp4[1]}")
except socket.timeout:
    print("  Timeout")

s.close()
print("\n=== Tests complete ===")
