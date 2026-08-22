"""UDP 配置协议 (端口 8600): 查询/参数校验/时间设置/静默丢弃。"""
import re
import socket
import struct
import time

import pytest

pytestmark = pytest.mark.functional

CMD_GET_VERSION = 0x04
CMD_SET_IP = 0x10
CMD_GET_IP = 0x11
CMD_SET_MODBUS = 0x12
CMD_GET_MODBUS = 0x13
CMD_SET_TIME = 0x14
CMD_FACTORY_RESET = 0x19


def udp_get_ip(dev):
    r = dev.udp_xfer(bytes([CMD_GET_IP]))
    return ".".join(str(b) for b in r[1:5])


def test_get_modbus_matches_info(dev):
    r = dev.udp_xfer(bytes([CMD_GET_MODBUS]))
    assert r[0] == CMD_GET_MODBUS and len(r) == 4, r.hex()
    slave, baud = r[1], struct.unpack(">H", r[2:4])[0]
    _, info = dev.http_json("GET", "/api/info")
    assert slave == info["slave_id"], (slave, info["slave_id"])
    assert baud == info["rs485_baud"], (baud, info["rs485_baud"])


def test_set_ip_invalid_rejected(dev):
    # ip_addr_valid: 首字节禁 0/127/>=224, 末字节禁 0/255
    for bad in ((0, 1, 1, 1), (127, 0, 0, 1), (224, 0, 0, 1), (255, 1, 1, 1),
                (192, 168, 12, 0), (192, 168, 12, 255)):
        r = dev.udp_xfer(bytes([CMD_SET_IP]) + bytes(bad))
        assert r == bytes([CMD_SET_IP, 0]), (bad, r.hex())
    assert udp_get_ip(dev) == dev.ip  # 状态未被改动


def test_set_ip_same_value_accepted(dev):
    a, b, c, d = (int(x) for x in dev.ip.split("."))
    r = dev.udp_xfer(bytes([CMD_SET_IP, a, b, c, d]))
    assert r == bytes([CMD_SET_IP, 1]), r.hex()
    assert udp_get_ip(dev) == dev.ip


def test_set_time_invalid_rejected(dev):
    for bad_ts in (0, 946684799, 4102444801):  # TS_MIN-1 .. TS_MAX+1
        r = dev.udp_xfer(bytes([CMD_SET_TIME])
                         + struct.pack(">I", bad_ts))
        assert r == bytes([CMD_SET_TIME, 0]), (bad_ts, r.hex())


def test_set_time_valid_and_readback(dev):
    now = int(time.time())
    r = dev.udp_xfer(bytes([CMD_SET_TIME]) + struct.pack(">I", now))
    assert r == bytes([CMD_SET_TIME, 1]), r.hex()
    _, info = dev.http_json("GET", "/api/info")
    assert abs(info["time"] - now) <= 10, (info["time"], now)


def test_set_modbus_same_value_accepted(dev):
    r = dev.udp_xfer(bytes([CMD_GET_MODBUS]))
    slave, baud = r[1], struct.unpack(">H", r[2:4])[0]
    r = dev.udp_xfer(bytes([CMD_SET_MODBUS, slave])
                     + struct.pack(">H", baud))
    assert r == bytes([CMD_SET_MODBUS, 1]), r.hex()
    r = dev.udp_xfer(bytes([CMD_GET_MODBUS]))
    assert (r[1], struct.unpack(">H", r[2:4])[0]) == (slave, baud)


def test_unknown_command_silent(dev):
    s = dev.udp(timeout=2.0)
    try:
        s.sendto(bytes([0x7F]), (dev.ip, 8600))
        with pytest.raises(socket.timeout):
            s.recvfrom(512)
    finally:
        s.close()


def test_factory_reset_first_step_rejected(dev):
    # 仅发送一次: 设备开机已远超 5s, 首步必须 ok=0 且不执行
    r = dev.udp_xfer(bytes([CMD_FACTORY_RESET]))
    assert r == bytes([CMD_FACTORY_RESET, 0]), r.hex()
    assert udp_get_ip(dev) == dev.ip  # 设备未被复位
