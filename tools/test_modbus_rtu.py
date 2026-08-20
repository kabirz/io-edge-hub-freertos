"""Test Modbus RTU over COM10 (CH340, USART2 @ 9600 baud)."""
import serial
import struct
import time

def crc16(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x0001:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc

def mb_rtu_request(port, slave, func, data=b''):
    pdu = bytes([slave, func]) + data
    crc = crc16(pdu)
    frame = pdu + struct.pack('<H', crc)
    port.reset_input_buffer()
    port.write(frame)
    port.flush()
    resp = port.read(256)
    return resp

print("=== Modbus RTU Test (COM10, 9600 8N1) ===\n")

port = serial.Serial('COM10', 9600, timeout=1)
time.sleep(0.1)

# FC03: Read 16 holding registers
print("--- FC03 Read Holding Registers (slave=1, 0x00..0x0F) ---")
resp = mb_rtu_request(port, 1, 3, struct.pack('>HH', 0, 16))
print(f"  Raw: {resp.hex()} ({len(resp)} bytes)")
if len(resp) >= 4:
    data = resp[:-2]
    rxcrc = struct.unpack('<H', resp[-2:])[0]
    calcrc = crc16(data)
    print(f"  CRC: {'OK' if rxcrc == calcrc else 'MISMATCH'}")
    fc = resp[1]
    if fc == 3:
        bc = resp[2]
        vals = struct.unpack(f'>{bc//2}H', resp[3:3+bc])
        print(f"  {len(vals)} registers read")
        for i, v in enumerate(vals):
            print(f"    [{i:02X}] = {v}")

# FC06: Write single register
print("\n--- FC06 Write Register (slave=1, reg=0x01, val=0xFFFF) ---")
resp2 = mb_rtu_request(port, 1, 6, struct.pack('>HH', 1, 0xFFFF))
print(f"  Raw: {resp2.hex()} ({len(resp2)} bytes)")
if len(resp2) >= 4:
    data2 = resp2[:-2]
    rxcrc2 = struct.unpack('<H', resp2[-2:])[0]
    calcrc2 = crc16(data2)
    print(f"  CRC: {'OK' if rxcrc2 == calcrc2 else 'MISMATCH'}")
    print(f"  FC=0x{resp2[1]:02X}")
    if resp2[1] == 6 and len(resp2) >= 8:
        reg, val = struct.unpack('>HH', resp2[2:6])
        print(f"  Echo: reg=0x{reg:02X}, val={val}")

port.close()
print("\n=== Tests complete ===")
