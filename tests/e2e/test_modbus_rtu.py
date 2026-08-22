"""Modbus RTU (USART2 经 USB-RS485): 需要 --rs485-port, 默认跳过。"""
import struct
import time

import pytest

from helpers.device import parse_version
from helpers.mbtcp import MbTcp  # noqa: F401 (marker 一致性)

pytestmark = [pytest.mark.functional, pytest.mark.serial, pytest.mark.rtu]


def crc16(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def rtu_xfer(port, slave, pdu, timeout=1.0):
    frame = bytes([slave]) + pdu
    frame += struct.pack("<H", crc16(frame))
    port.reset_input_buffer()
    port.write(frame)
    port.flush()
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        n = port.in_waiting
        if n:
            buf += port.read(n)
            if len(buf) >= 4:
                expect = 3 + buf[2] + 2  # addr+fc+bc+crc (读响应)
                if len(buf) >= expect:
                    break
        time.sleep(0.01)
    assert len(buf) >= 4, buf.hex()
    assert struct.unpack("<H", buf[-2:])[0] == crc16(buf[:-2]), buf.hex()
    return buf


@pytest.fixture
def rs485(request, dev):
    import serial

    port_name = request.config.getoption("--rs485-port")
    if not port_name:
        pytest.skip("--rs485-port not given: RTU tests disabled")
    _, info = dev.http_json("GET", "/api/info")
    baud = request.config.getoption("--rs485-baud") or info["rs485_baud"]
    return serial.Serial(port_name, baud, timeout=0.1), info["slave_id"]


def test_rtu_fc03_read(rs485):
    port, slave = rs485
    resp = rtu_xfer(port, slave, struct.pack(">BHH", 3, 0, 18))
    assert resp[1] == 3 and resp[2] == 36, resp.hex()


def test_rtu_fc04_version(rs485, dev):
    port, slave = rs485
    resp = rtu_xfer(port, slave, struct.pack(">BHH", 4, 0, 1))
    val = struct.unpack(">H", resp[3:5])[0]
    udp = parse_version(dev.udp_xfer(b"\x04"))
    major, minor, patch = (int(x) for x in udp.split("_")[0][1:].split("."))
    assert val == (major << 12) | (minor << 8) | patch, (val, udp)


def test_rtu_fc06_same_value_write(rs485):
    port, slave = rs485
    resp = rtu_xfer(port, slave, struct.pack(">BHH", 3, 1, 1))
    cur = struct.unpack(">H", resp[3:5])[0]
    resp = rtu_xfer(port, slave, struct.pack(">BHH", 6, 1, cur))
    assert resp[1] == 6 and struct.unpack(">HH", resp[2:6]) == (1, cur)
