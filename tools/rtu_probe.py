"""Modbus RTU smoke probe on COM10: FC03 read holding, try baud rates."""
import serial
import time


def crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def frame(addr, fc, data):
    body = bytes([addr, fc]) + data
    c = crc16(body)
    return body + bytes([c & 0xFF, c >> 8])


for baud in (9600, 115200, 19200, 38400, 57600):
    s = serial.Serial("COM10", baud, timeout=0.5)
    time.sleep(0.1)
    s.reset_input_buffer()
    # FC03: read holding reg 9 (slave id), 1 register
    s.write(frame(1, 3, bytes([0x00, 0x09, 0x00, 0x01])))
    r = s.read(64)
    s.close()
    if len(r) >= 7 and r[0] == 1 and r[1] == 3:
        print(f"RTU OK @ {baud}: {r.hex()}")
        break
    print(f"@{baud}: no reply ({len(r)} bytes)")
else:
    print("RTU: no baud matched")
