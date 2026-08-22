"""Modbus TCP 功能测试: 读/写/诊断/异常/广播/静默丢弃/流水线/连接限制。"""
import socket
import struct
import time

import pytest

from helpers.mbtcp import MbTcp, adu, exc_code, regs

pytestmark = pytest.mark.functional


def test_read_all_holding(mb, dev):
    vals = regs(mb.req(MbTcp.read_req(3, 0, 18)))
    assert len(vals) == 18
    _, info = dev.http_json("GET", "/api/info")
    ip = [int(x) for x in dev.ip.split(".")]
    assert vals[0x09] == info["slave_id"]
    assert vals[0x0A:0x0E] == ip


def test_read_all_input(mb):
    vals = regs(mb.req(MbTcp.read_req(4, 0, 6)))
    assert len(vals) == 6


def test_time_registers_live(mb, dev):
    _, info = dev.http_json("GET", "/api/info")
    hi, lo = regs(mb.req(MbTcp.read_req(3, 0x0E, 2)))
    live = (hi << 16) | lo
    assert abs(live - info["time"]) <= 3, (live, info["time"])


def test_coils_and_discrete_mirror_io(mb, dev):
    _, io = dev.http_json("GET", "/api/io")
    coils = regs(mb.req(MbTcp.read_req(1, 0, 8)))[:8]
    assert coils == io["do"], (coils, io["do"])
    discrete = regs(mb.req(MbTcp.read_req(2, 0, 16)))[:16]
    assert discrete == io["di"], (discrete, io["di"])


def test_fc05_coil_write_readback(mb, dev):
    try:
        regs(mb.req(MbTcp.write_coil(5, True)))
        coils = regs(mb.req(MbTcp.read_req(1, 0, 8)))
        assert coils[5] == 1
        _, io = dev.http_json("GET", "/api/io")
        assert io["do"][5] == 1, io["do"]
    finally:
        mb.req(MbTcp.write_coil(5, False))
    assert regs(mb.req(MbTcp.read_req(1, 0, 8)))[5] == 0


def test_fc06_write_readback(mb, dev):
    try:
        regs(mb.req(MbTcp.write_reg(0, 0x0055)))
        assert regs(mb.req(MbTcp.read_req(3, 0, 1)))[0] == 0x0055
        _, io = dev.http_json("GET", "/api/io")
        assert io["do"] == [1 if 0x55 >> i & 1 else 0 for i in range(8)], \
            io["do"]
    finally:
        mb.req(MbTcp.write_reg(0, 0x0000))
    assert regs(mb.req(MbTcp.read_req(3, 0, 1)))[0] == 0


def test_fc16_write_multiple(mb):
    cur = regs(mb.req(MbTcp.read_req(3, 1, 2)))
    pdu = mb.req(MbTcp.write_regs(1, cur))  # 原值回写: 无副作用
    assert struct.unpack(">HH", pdu[1:5]) == (1, 2)
    assert regs(mb.req(MbTcp.read_req(3, 1, 2))) == cur


def test_fc08_diagnostics(mb):
    pdu = mb.req(bytes([8]) + struct.pack(">HH", 0x0000, 0xA5A5))  # 回显
    assert struct.unpack(">HH", pdu[1:5]) == (0x0000, 0xA5A5), pdu.hex()
    mb.req(bytes([8]) + struct.pack(">HH", 0x000A, 0x0000))  # 清计数器
    mb.req(MbTcp.read_req(3, 0, 1))
    mb.req(MbTcp.read_req(3, 0, 1))
    pdu = mb.req(bytes([8]) + struct.pack(">HH", 0x000B, 0x0000))
    assert exc_code(pdu) is None, pdu.hex()
    assert struct.unpack(">H", pdu[3:5])[0] >= 2, pdu.hex()  # 已计数


def test_exceptions(mb):
    assert exc_code(mb.req(MbTcp.read_req(3, 18, 1))) == 0x02  # 越界地址
    assert exc_code(mb.req(MbTcp.read_req(3, 0, 126))) == 0x03  # 数量超限
    assert exc_code(mb.req(bytes([0x41, 0x00]))) == 0x01  # 未知功能码
    assert exc_code(mb.req(MbTcp.read_req(6, 5000, 1))) == 0x02  # FP 扩展区
    assert exc_code(mb.req(MbTcp.read_req(3, 5000, 1))) == 0x01
    pdu = mb.req(MbTcp.read_req(3, 0, 1), proto=1)  # 非 0 协议号 -> 异常
    assert pdu[0] & 0x80 and exc_code(pdu), pdu.hex()


def test_broadcast_no_reply(mb, dev):
    mb.sock.sendall(adu(99, 0, MbTcp.read_req(3, 0, 1)))  # unit=0 广播
    mb.sock.settimeout(1.5)
    with pytest.raises(socket.timeout):
        mb.sock.recv(260)
    mb.sock.settimeout(5)
    regs(mb.req(MbTcp.read_req(3, 0, 1)))  # 连接仍然可用


def test_truncated_pdu_silent_drop(mb):
    # MBAP 声明 2 字节但 PDU 不完整: 必须静默丢弃且连接存活
    mb.sock.sendall(struct.pack(">HHHB", 7, 0, 2, 1) + bytes([3]))
    mb.sock.settimeout(1.5)
    with pytest.raises(socket.timeout):
        mb.sock.recv(260)
    mb.sock.settimeout(5)
    regs(mb.req(MbTcp.read_req(3, 0, 1)))


def test_pipelined_requests(mb):
    mb.sock.sendall(adu(5, 1, MbTcp.read_req(3, 0, 1))
                    + adu(6, 1, MbTcp.read_req(3, 1, 1)))
    mb.sock.settimeout(5)
    buf = b""
    replies = []
    while len(replies) < 2:
        chunk = mb.sock.recv(260)
        if not chunk:
            break
        buf += chunk
        while len(buf) >= 6:
            n = 6 + ((buf[4] << 8) | buf[5])
            if len(buf) < n:
                break
            replies.append(buf[:n])
            buf = buf[n:]
    assert len(replies) == 2
    assert replies[0][:2] == b"\x00\x05" and replies[1][:2] == b"\x00\x06"
    assert replies[0][7] == 3 and replies[1][7] == 3


def test_max_two_masters(dev, fw_kind):
    if fw_kind != "freertos":
        pytest.skip("2-master cap is an mb-tcp-port policy "
                    "(Zephyr config serves more)")
    m1, m2 = MbTcp(dev), MbTcp(dev)
    try:
        regs(m1.req(MbTcp.read_req(3, 0, 1)))
        regs(m2.req(MbTcp.read_req(3, 0, 1)))
        m3 = MbTcp(dev)
        try:
            with pytest.raises((ConnectionError, socket.timeout,
                                TimeoutError, AssertionError)):
                m3.sock.sendall(adu(1, 1, MbTcp.read_req(3, 0, 1)))
                m3.sock.settimeout(3)
                data = m3.sock.recv(260)
                assert len(data) >= 8 and data[7] == 3, \
                    f"3rd master served: {data.hex()}"
        finally:
            m3.close()
    finally:
        m1.close()
        m2.close()
