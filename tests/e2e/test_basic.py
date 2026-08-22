"""基础测试: 连通性 + 各服务入口冒烟 (ICMP/TCP/UDP/HTTP/FTP/Modbus)。"""
import re
import subprocess
import time

import pytest

from helpers.device import parse_version, read_http_message
from helpers.mbtcp import regs

pytestmark = pytest.mark.basic


def test_ping(dev):
    out = subprocess.run(["ping", "-n", "3", "-w", "1000", dev.ip],
                         capture_output=True, text=True, errors="replace"
                         ).stdout
    assert "TTL=" in out or "ttl=" in out, out


def test_tcp_ftp_banner(dev):
    s = dev.tcp(21, timeout=5)
    try:
        banner = s.recv(256)
    finally:
        s.close()
    assert banner == b"220 io-edge-hub FTP service ready\r\n", banner


def test_tcp_http_responds(dev):
    st, _, body = dev.http("GET", "/api/info")
    assert "200" in st, st
    assert b'"t":"info"' in body, body[:120]


def test_tcp_modbus_responds(dev):
    from helpers.mbtcp import MbTcp
    with MbTcp(dev) as m:
        pdu = m.req(MbTcp.read_req(3, 0, 1))
        assert pdu[0] == 3 and regs(pdu)[0] is not None


def test_udp_get_version(dev):
    v = parse_version(dev.udp_xfer(b"\x04"))
    assert re.match(r"^v\d+\.\d+\.\d+_", v), v


def test_udp_get_ip(dev):
    r = dev.udp_xfer(b"\x11")
    assert r[0] == 0x11 and len(r) == 5, r.hex()
    assert ".".join(str(b) for b in r[1:5]) == dev.ip, r.hex()


def test_http_info_fields(dev):
    st, info = dev.http_json("GET", "/api/info")
    assert "200" in st, st
    assert info["t"] == "info"
    assert info["board"] == "io_edge_f407vet6"
    assert info["hclk_mhz"] == 168
    assert info["flash_kb"] == 512
    assert info["sram_kb"] == 192
    assert info["ip"] == dev.ip
    assert info["net_up"] is True
    assert re.match(r"^v\d+\.\d+\.\d+_", info["version"]), info["version"]
    assert info["uptime_ms"] > 0
    assert 0 <= info["hist_en"] <= 1
    assert 0 < info["lfs_free"] <= info["lfs_total"]


def test_version_consistent_across_surfaces(dev):
    udp_v = parse_version(dev.udp_xfer(b"\x04"))
    _, info = dev.http_json("GET", "/api/info")
    prefix = udp_v.split("_")[0]  # 'vM.m.p' 部分 (git 截断长度不同)
    assert info["version"].startswith(prefix + "_"), (udp_v, info["version"])

    major, minor, patch = (int(x) for x in prefix[1:].split("."))
    from helpers.mbtcp import MbTcp
    with MbTcp(dev) as m:  # Modbus 输入寄存器 0 = MAJOR<<12|MINOR<<8|PATCH
        val = regs(m.req(MbTcp.read_req(4, 0, 1)))[0]
    assert val == (major << 12) | (minor << 8) | patch, (val, prefix)


def test_history_list_reachable(dev):
    st, h = dev.http_json("GET", "/api/history")
    assert "200" in st and "files" in h, h
    for f in h["files"]:
        assert f["name"].startswith("data_"), f
        assert f["size"] >= 0, f
