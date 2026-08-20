import socket
import struct

IP = '192.168.12.101'
PORT = 502

def mbap_request(tid, uid, pdu_data):
    """Build Modbus TCP request frame."""
    length = 1 + len(pdu_data)  # unit_id + PDU
    header = struct.pack('>HHHB', tid, 0, length, uid)
    return header + pdu_data

def mbap_read(s, uid, func, start, count, tid=1):
    pdu = struct.pack('>BHH', func, start, count)
    s.sendall(mbap_request(tid, uid, pdu))
    return s.recv(256)

print("=== Modbus TCP Tests ===\n")

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(3)
s.connect((IP, PORT))
print("Connected to port 502\n")

# FC03: Read 16 holding registers (0x00-0x0F)
print("--- FC03 Read Holding Registers (0x00-0x0F) ---")
resp = mbap_read(s, 1, 3, 0, 16, tid=1)
if len(resp) >= 9:
    tid, proto, length, uid = struct.unpack('>HHHB', resp[:7])
    fc = resp[7]
    if fc & 0x80:
        print(f"  Exception: FC=0x{fc:02X}, code={resp[8]}")
    else:
        bc = resp[8]
        vals = struct.unpack(f'>{bc//2}H', resp[9:9+bc])
        names = ['DI_IN', 'DOUT_OUT', 'AI1_L', 'AI1_H', 'AI2_L', 'AI2_H',
                 'AI3_L', 'AI3_H', 'AI4_L', 'AI4_H', 'TS_HI', 'TS_LO',
                 'IP1', 'IP2', 'IP3', 'IP4']
        for i, v in enumerate(vals):
            nm = names[i] if i < len(names) else f'reg_{i:02X}'
            print(f"  [{i:02X}] {nm:12s} = {v} (0x{v:04X})")
else:
    print(f"  Short resp: {resp.hex()}")

# FC06: Write single register (test write)
print("\n--- FC06 Write Single Register (0x0D=101) ---")
pdu_w = struct.pack('>BHH', 6, 0x0D, 101)  # FC06, reg 0x0D, value 101
s.sendall(mbap_request(0x0002, 1, pdu_w))
resp2 = s.recv(256)
if len(resp2) >= 8:
    fc2 = resp2[7]
    if fc2 == 6:
        reg, val = struct.unpack('>HH', resp2[8:12])
        print(f"  OK: wrote reg 0x{reg:02X} = {val}")
    elif fc2 & 0x80:
        print(f"  Exception: code={resp2[8]}")
    else:
        print(f"  Unexpected FC: 0x{fc2:02X}")

# FC03: Read version info (registers 0x14-0x15)
print("\n--- FC03 Read FW Version (0x14-0x17) ---")
resp3 = mbap_read(s, 1, 3, 0x14, 4, tid=3)
if len(resp3) >= 9:
    fc3 = resp3[7]
    if fc3 & 0x80:
        print(f"  Exception: code={resp3[8]}")
    else:
        bc3 = resp3[8]
        vals3 = struct.unpack(f'>{bc3//2}H', resp3[9:9+bc3])
        for i, v in enumerate(vals3):
            print(f"  [{0x14+i:02X}] = {v} (0x{v:04X})")

s.close()
print("\n=== Tests complete ===")
